/*
 * ============================================================================
 *  WayCare Dock  -  Firmware DEFINITIVO (hardware real)
 * ============================================================================
 *  Diferenças vs. versão Wokwi:
 *    - WiFi real (SSID + senha)
 *    - HX_SCALE_FACTOR vem da CALIBRAÇÃO (rode waycare_calibracao.ino antes)
 *    - Tara só pelo site (sem botão físico)
 *    - scale.set_scale(fator) direto (no Wokwi era 1/fator por causa do bruto fake)
 *
 *  Mesma lógica de borda: estabiliza o peso e publica só na mudança;
 *  recebe comando de tara/LED da nuvem via MQTT.
 * ============================================================================
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include "HX711.h"

// ---------------------------------------------------------------------------
// CONFIGURAÇÃO  (PREENCHA antes de subir)
// ---------------------------------------------------------------------------
const char* WIFI_SSID  = "joaovictor";
const char* WIFI_PASS  = "Jv102030";

const char* MQTT_HOST  = "52.224.244.229";  // IP publico da VM
const int   MQTT_PORT  = 1883;

const char* APIKEY     = "waycare2025";
const char* DEVICE_ID  = "dock01";

const char* TOPIC_ATTRS = "/json/waycare2025/dock01/attrs"; // dados -> FIWARE
const char* TOPIC_CMD   = "waycare/dock01/cmd";             // comandos <- Python

// ---------------------------------------------------------------------------
// PINAGEM
// ---------------------------------------------------------------------------
#define HX_DT   16
#define HX_SCK  17
#define LED_R   12
#define LED_G   14
#define LED_B   27

// ---------------------------------------------------------------------------
// CALIBRAÇÃO  (COLE AQUI o fator que o waycare_calibracao.ino imprimiu)
// ---------------------------------------------------------------------------
#define HX_SCALE_FACTOR  442.3777f   // valor calibrado da SUA celula

// ---------------------------------------------------------------------------
// PARÂMETROS DE LEITURA
// ---------------------------------------------------------------------------
#define ESTABILIZA_MS    3000     // tempo parado (na faixa) p/ considerar estável
#define TOLERANCIA_G     8.0f     // faixa do "parado": ruído menor que isso = estável
#define MIN_DELTA_G      10.0f    // só publica novo peso se mudar 10g+ do último
#define MAX_JANELA       60       // amostras coletadas na janela estável (3s @ ~10Hz)

// ---------------------------------------------------------------------------
// ESTADO INTERNO
// ---------------------------------------------------------------------------
HX711 scale;
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

float  ultimoPesoLido = 0.0f;     // cache (HX711 entrega ~10Hz)
float  pesoPublicado  = -9999.0;  // último peso ENVIADO ao backend
float  refEstavel     = 0.0f;     // referência da faixa de estabilização
unsigned long inicioEstavel = 0;
unsigned long ultimaLeituraHX = 0;

float  janela[MAX_JANELA];        // leituras coletadas durante a janela estável
int    janelaCount = 0;
bool   publicadoNestaJanela = false;

// ===========================================================================
//  LED RGB  -  CÁTODO COMUM (COM no GND): valor direto, SEM "255 -".
//  Se um dia trocar por ânodo comum (COM no 3V3), volte a usar (255 - valor).
// ===========================================================================
void setRGB(int r, int g, int b) {
  analogWrite(LED_R, r);
  analogWrite(LED_G, g);
  analogWrite(LED_B, b);
}

void aplicarCorLED(const String& cor) {
  if      (cor == "verde")          setRGB(0, 255, 0);
  else if (cor == "verde_pulsando") setRGB(0, 255, 80);
  else if (cor == "vermelho")       setRGB(255, 0, 0);
  else if (cor == "azul")           setRGB(0, 0, 255);
  else                              setRGB(0, 40, 0);
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

void onComando(char* topic, byte* payload, unsigned int len) {
  String msg;
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
  Serial.print("CMD recebido: "); Serial.println(msg);

  if (msg.indexOf("tara") >= 0) {
    setRGB(0, 0, 255);             // azul = tarando
    scale.tare(20);
    delay(400);
    pesoPublicado = -9999.0;       // força republicar pós-tara
    Serial.println(">> TARA executada");
  }

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
      mqtt.subscribe(TOPIC_CMD);
    } else {
      Serial.print(" falhou rc="); Serial.println(mqtt.state());
      delay(2000);
    }
  }
}

// ===========================================================================
//  LEITURA + ESTABILIZAÇÃO (por mediana da janela)
// ===========================================================================
// Lê o peso bruto (cacheado). NÃO corta valores negativos: peso negativo é o
// sinal de "garrafa fora da dock" (a tara é feita com a garrafa vazia em cima,
// então remover a garrafa deixa o peso negativo).
float lerPeso() {
  if (scale.is_ready()) {
    ultimoPesoLido = scale.get_units(1);
  }
  return ultimoPesoLido;
}

// Mediana de um vetor (ordena in-place). Robusta a ruído: ignora os extremos
// e devolve o valor central — o "valor que mais representa" a faixa estável.
float mediana(float* v, int n) {
  for (int i = 1; i < n; i++) {       // insertion sort (n pequeno, ~30)
    float k = v[i]; int j = i - 1;
    while (j >= 0 && v[j] > k) { v[j + 1] = v[j]; j--; }
    v[j + 1] = k;
  }
  if (n == 0) return 0;
  return (n % 2) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0f;
}

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
  setRGB(0, 0, 255);

  scale.begin(HX_DT, HX_SCK);
  scale.set_scale(HX_SCALE_FACTOR);   // fator REAL calibrado (direto, não invertido)
  scale.tare(20);                     // zera no boot (dock vazia ao ligar!)

  conectarWiFi();
  conectarMQTT();
  setRGB(0, 40, 0);
  inicioEstavel = millis();
}

void loop() {
  if (!mqtt.connected()) conectarMQTT();
  mqtt.loop();

  if (millis() - ultimaLeituraHX < 100) return;   // ~10Hz
  ultimaLeituraHX = millis();

  float peso = lerPeso();

  // Saiu da faixa de tolerância -> o peso está MUDANDO (gole, abastecimento,
  // garrafa sendo manuseada). Reinicia a janela de estabilização.
  if (fabs(peso - refEstavel) > TOLERANCIA_G) {
    refEstavel = peso;
    inicioEstavel = millis();
    janelaCount = 0;
    publicadoNestaJanela = false;
    return;
  }

  // Dentro da faixa: o peso está "parado" (só com ruído de ~1g). Coleta amostra.
  if (janelaCount < MAX_JANELA) janela[janelaCount++] = peso;

  // Ficou parado tempo suficiente e ainda não publicou esta janela?
  if (!publicadoNestaJanela && millis() - inicioEstavel >= ESTABILIZA_MS) {
    float estavel = mediana(janela, janelaCount);   // valor limpo da faixa

    // Só publica se mudou o bastante em relação ao último enviado.
    // (A mediana ignora o ruído de ±1g, então peso "parado" não republica.)
    if (fabs(estavel - pesoPublicado) >= MIN_DELTA_G) {
      publicarPeso(estavel);
      pesoPublicado = estavel;
    }
    publicadoNestaJanela = true;   // não republica a mesma janela a cada ciclo
  }
}
