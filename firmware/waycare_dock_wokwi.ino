/*
 * ============================================================================
 *  WayCare Dock  -  Firmware ESP32 (camada de borda / Edge)
 * ============================================================================
 *  FILOSOFIA (importante para defender na banca):
 *  Este firmware NÃO calcula consumo, %, fatias ou gamificação.
 *  Essa lógica de negócio mora no backend Python (nuvem).
 *
 *  O edge faz APENAS o que exige o stream rápido do sensor:
 *    1) Lê a célula de carga via HX711 a ~10Hz
 *    2) Suaviza o ruído (média móvel) e detecta quando o peso ESTABILIZA
 *    3) Publica o peso estável via MQTT SÓ QUANDO ele muda (event-driven)
 *       -> isso evita inundar o broker com leituras a 10Hz
 *    4) Recebe comandos da nuvem: tara (re-zera) e cor do LED
 *
 *  Fluxo de dados:  ESP32 -> MQTT(/json/.../attrs) -> IoT Agent -> Orion
 *  Fluxo de comando: Python -> MQTT(waycare/dock01/cmd) -> ESP32
 * ============================================================================
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include "HX711.h"

// ---------------------------------------------------------------------------
// CONFIGURAÇÃO  (ajuste o IP da sua VM antes de compilar)
// ---------------------------------------------------------------------------
const char* WIFI_SSID  = "Wokwi-GUEST";   // rede padrão do simulador Wokwi
const char* WIFI_PASS  = "";

const char* MQTT_HOST  = "SEU_IP_DA_VM";  // <<< TROQUE pelo IP público da VM
const int   MQTT_PORT  = 1883;

const char* APIKEY     = "waycare2025";
const char* DEVICE_ID  = "dock01";

// Tópico de DADOS (sobe pro FIWARE via IoT Agent JSON)
const char* TOPIC_ATTRS = "/json/waycare2025/dock01/attrs";
// Tópico de COMANDO (Python publica aqui; o ESP32 escuta)
const char* TOPIC_CMD   = "waycare/dock01/cmd";

// ---------------------------------------------------------------------------
// PINAGEM  (mesma do diagram.json)
// ---------------------------------------------------------------------------
#define HX_DT   16
#define HX_SCK  17
#define LED_R   12
#define LED_G   14
#define LED_B   27

// ---------------------------------------------------------------------------
// PARÂMETROS DE LEITURA
// ---------------------------------------------------------------------------
#define HX_SCALE_FACTOR  0.42f   // calibração Wokwi: 2100/5000 = 0.42 (g)
#define N_MEDIA          20      // amostras da média móvel
#define ESTABILIZA_MS    3000    // tempo parado pra considerar "estável"
#define TOLERANCIA_G     8.0f    // variação máx. (g) p/ contar como "parado"
#define MIN_DELTA_G      20.0f   // mudança mínima p/ publicar novo peso

// ---------------------------------------------------------------------------
// ESTADO INTERNO
// ---------------------------------------------------------------------------
HX711 scale;
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

float  buffer[N_MEDIA];          // buffer circular da média móvel
int    idxBuf = 0;
bool   bufCheio = false;

float  ultimoPesoLido = 0.0f;    // cache (HX711 só entrega ~10Hz)
float  pesoPublicado  = -9999.0; // último peso ENVIADO ao backend
float  refEstavel     = 0.0f;    // valor de referência da janela de estabilização
unsigned long inicioEstavel = 0; // quando a janela "parada" começou

unsigned long ultimaLeituraHX = 0;

// ===========================================================================
//  LED RGB  (ânodo comum -> lógica invertida: 255 - valor)
// ===========================================================================
void setRGB(int r, int g, int b) {
  analogWrite(LED_R, 255 - r);
  analogWrite(LED_G, 255 - g);
  analogWrite(LED_B, 255 - b);
}

// Aplica a cor recebida da nuvem
void aplicarCorLED(const String& cor) {
  if      (cor == "verde")          setRGB(0, 255, 0);
  else if (cor == "verde_pulsando") setRGB(0, 255, 80);   // (pulso real é opcional)
  else if (cor == "vermelho")       setRGB(255, 0, 0);
  else if (cor == "azul")           setRGB(0, 0, 255);
  else                              setRGB(0, 40, 0);     // verde fraco = ocioso
}

// ===========================================================================
//  WIFI / MQTT
// ===========================================================================
void conectarWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Conectando WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(300); Serial.print("."); }
  Serial.println(" OK");
}

// Callback de comandos vindos do backend
void onComando(char* topic, byte* payload, unsigned int len) {
  String msg;
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
  Serial.print("CMD recebido: "); Serial.println(msg);

  // ---- Comando de TARA: re-zera a balança ----
  if (msg.indexOf("tara") >= 0) {
    setRGB(0, 0, 255);            // azul sólido = tarando
    scale.tare(20);              // média de 20 leituras como novo zero
    delay(400);
    pesoPublicado = -9999.0;     // força republicar o peso pós-tara
    Serial.println(">> TARA executada");
  }

  // ---- Comando de LED: extrai a cor entre aspas após "led" ----
  int p = msg.indexOf("led");
  if (p >= 0) {
    int a1 = msg.indexOf('"', msg.indexOf(':', p));
    int a2 = msg.indexOf('"', a1 + 1);
    if (a1 >= 0 && a2 > a1) aplicarCorLED(msg.substring(a1 + 1, a2));
  }
}

void conectarMQTT() {
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onComando);
  while (!mqtt.connected()) {
    Serial.print("Conectando MQTT...");
    String cid = "waycare-" + String(DEVICE_ID);
    if (mqtt.connect(cid.c_str())) {
      Serial.println(" OK");
      mqtt.subscribe(TOPIC_CMD);          // passa a ouvir comandos
    } else {
      Serial.print(" falhou rc="); Serial.println(mqtt.state());
      delay(2000);
    }
  }
}

// ===========================================================================
//  LEITURA + ESTABILIZAÇÃO
// ===========================================================================
// Lê o HX711 (quando disponível) e devolve a média móvel atual em gramas.
float lerPesoMedio() {
  if (scale.is_ready()) {
    float g = scale.get_units(1);          // 1 leitura, já em gramas (factor)
    if (g < 0) g = 0;                      // sem peso negativo
    ultimoPesoLido = g;
    buffer[idxBuf] = g;
    idxBuf = (idxBuf + 1) % N_MEDIA;
    if (idxBuf == 0) bufCheio = true;
  }
  // média do que já temos no buffer
  int n = bufCheio ? N_MEDIA : idxBuf;
  if (n == 0) return ultimoPesoLido;
  float soma = 0;
  for (int i = 0; i < n; i++) soma += buffer[i];
  return soma / n;
}

// Publica o peso estável no FIWARE
void publicarPeso(float peso) {
  char json[48];
  snprintf(json, sizeof(json), "{\"peso\":%.1f}", peso);
  mqtt.publish(TOPIC_ATTRS, json);
  Serial.print("Publicado -> "); Serial.println(json);
}

// ===========================================================================
//  SETUP / LOOP
// ===========================================================================
void setup() {
  Serial.begin(115200);
  pinMode(LED_R, OUTPUT); pinMode(LED_G, OUTPUT); pinMode(LED_B, OUTPUT);
  setRGB(0, 0, 255);                       // azul no boot

  scale.begin(HX_DT, HX_SCK);
  scale.set_scale(1.0f / HX_SCALE_FACTOR); // converte bruto -> gramas
  scale.tare(20);                          // zera no boot

  conectarWiFi();
  conectarMQTT();
  setRGB(0, 40, 0);                        // verde fraco = pronto/ocioso
  inicioEstavel = millis();
}

void loop() {
  if (!mqtt.connected()) conectarMQTT();
  mqtt.loop();

  // limita a ~10Hz a leitura do sensor
  if (millis() - ultimaLeituraHX < 100) return;
  ultimaLeituraHX = millis();

  float peso = lerPesoMedio();

  // --- Detector de estabilização ---
  // Se o peso atual está dentro da tolerância da referência, a janela continua.
  // Se saiu da tolerância, reinicia a janela com o novo valor.
  if (fabs(peso - refEstavel) > TOLERANCIA_G) {
    refEstavel = peso;
    inicioEstavel = millis();
    return;                                // ainda mexendo, não publica
  }

  // Peso parado por tempo suficiente?
  if (millis() - inicioEstavel >= ESTABILIZA_MS) {
    // Só publica se mudou o bastante em relação ao último enviado
    if (fabs(peso - pesoPublicado) >= MIN_DELTA_G) {
      publicarPeso(peso);
      pesoPublicado = peso;
    }
  }
}
