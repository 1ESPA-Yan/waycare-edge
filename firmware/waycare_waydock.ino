//João Victor Melo Santos RM: 566640
//Yan Lucas Gonçalves da Silva RM: 567046
//Gustavo Atsuyuki Hiruo RM: 567625
//Gustavo Macedo Daniel RM: 567594

// ============================================================
//  WayCare Dock — Simulação Wokwi + MQTT / Fiware
//  Hardware: ESP32 DevKit C v4
//  Componentes: Célula de carga + HX711 (peso real),
//               Pushbutton (tara), RGB LED (feedback visual)
//
//  Conecta ao Wi-Fi e publica dados de hidratação via MQTT
//  no formato JSON para o IoT Agent do Fiware (iotagent-json),
//  que repassa ao Orion Context Broker.
// ============================================================

#include <WiFi.h>
#include <PubSubClient.h>
#include "HX711.h"

// ── Wi-Fi ─────────────────────────────────────────────────────
// No Wokwi, usar "Wokwi-GUEST" sem senha
#define WIFI_SSID     "Wokwi-GUEST"
#define WIFI_PASSWORD ""

// ── Broker MQTT ───────────────────────────────────────────────
// IP público da VM na Azure onde o Mosquitto está rodando
// Porta 1883 = MQTT padrão sem criptografia
#define MQTT_BROKER   "52.224.244.229"
#define MQTT_PORT     1883
#define MQTT_USER     ""
#define MQTT_PASSWORD ""

// ── Fiware IoT Agent ──────────────────────────────────────────
// Apikey e device_id cadastrados no IoT Agent via Postman
// Tópico de publicação: /json/<apikey>/<device_id>/attrs
#define FIWARE_APIKEY   "waycare2025"
#define DEVICE_ID       "dock01"

// ── Pinagem ──────────────────────────────────────────────────
#define PIN_HX_DT   16
#define PIN_HX_SCK  17
#define PIN_BTN   25
#define PIN_R     12
#define PIN_G     14
#define PIN_B     27

// Fator de calibração da HX711.
// IMPORTANTE: O Wokwi tem um bug conhecido — para uma célula de 5kg,
// ele entrega valor bruto máximo de ~2100 (em vez de ~2.100.000 do HX711 real).
// Como queremos que 0-5kg apareça como 0-5000g, usamos:
//   set_scale = 2100 / 5000 = 0.42
// Em hardware real, este fator viria da calibração com peso conhecido.
#define HX_SCALE_FACTOR  0.42f

// ── Configurações de peso ─────────────────────────────────────
// Célula de carga do Wokwi suporta até 5kg = 5000g
#define MAX_WEIGHT_G        5000
#define TARE_HOLD_MS        2000
#define STABLE_READS        20
#define STABLE_TIME_MS      3000
#define AIRBORNE_THRESHOLD  30
#define AIRBORNE_RETURN_MS  4000
#define MIN_DELTA_G         20

// ── Meta diária de hidratação ─────────────────────────────────
#define DAILY_GOAL_ML       2000
#define GOAL_STEPS          4
#define STEP_CELEBRATE_MS   5000

// ── Alerta de sedentarismo hídrico ───────────────────────────
#define IDLE_ALERT_MS      (10UL * 60UL * 1000UL)

// ── PWM ──────────────────────────────────────────────────────
#define PWM_FREQ   5000
#define PWM_RES    8

// ── Heartbeat MQTT ────────────────────────────────────────────
// Publica o estado atual a cada 30s mesmo sem novo consumo,
// para manter dados contínuos no histórico do Fiware
#define MQTT_REPORT_MS  30000UL

// ── Objetos MQTT ─────────────────────────────────────────────
WiFiClient   wifiClient;
PubSubClient mqttClient(wifiClient);

unsigned long lastMqttReportMs = 0;
bool          mqttEverConnected = false;

// ── Objeto da célula de carga (HX711) ────────────────────────
HX711 scale;

// ── Estado global ─────────────────────────────────────────────
float   tareOffset      = 0;
float   lastStableG     = 0;
float   lastRawG        = 0;   // última leitura válida da HX711 (cache)
float   readings[STABLE_READS];
int     readIndex       = 0;
float   readSum         = 0;

float   stableCandidate = 0;
unsigned long stableStart = 0;
bool    waitingStable   = false;

bool    isAirborne      = false;
unsigned long airborneStart = 0;

float   totalConsumedG  = 0;
int     goalsReached    = 0;

unsigned long lastDrinkMs  = 0;
unsigned long celebrateEnd = 0;
bool          taredOnce    = false;

unsigned long btnPressStart = 0;
bool          btnWasPressed  = false;
bool          taringNow      = false;

unsigned long lastLogMs = 0;
#define LOG_INTERVAL_MS 500

// ── Funções de cor do LED RGB ─────────────────────────────────
// IMPORTANTE: o LED RGB do Wokwi é ÂNODO COMUM (pino central em 3V3).
// Isso significa que o PWM funciona invertido: 0 = totalmente aceso,
// 255 = totalmente apagado. Por isso aplicamos (255 - valor) abaixo.
void setRGB(uint8_t r, uint8_t g, uint8_t b) {
  ledcWrite(PIN_R, 255 - r);
  ledcWrite(PIN_G, 255 - g);
  ledcWrite(PIN_B, 255 - b);
}
void ledOff()    { setRGB(0,   0,   0);   }
void ledRed()    { setRGB(255, 0,   0);   }
void ledGreen()  { setRGB(0,   255, 0);   }
void ledBlue()   { setRGB(0,   0,   255); }
void ledWhite()  { setRGB(255, 255, 255); }
void ledYellow() { setRGB(255, 180, 0);   }

// ── Conexão Wi-Fi ─────────────────────────────────────────────
void connectWiFi() {
  Serial.printf("\n[WiFi] Conectando a '%s'...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 30) {
    delay(500);
    Serial.print(".");
    tries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Conectado! IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[WiFi] FALHA — continuando sem MQTT.");
  }
}

// ── Conexão MQTT com reconexão automática ────────────────────
void connectMQTT() {
  if (WiFi.status() != WL_CONNECTED) return;

  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);

  // ClientId único para evitar conflito de conexões no broker
  String clientId = "WayCare-" + String(DEVICE_ID) + "-" + String(millis());

  Serial.printf("[MQTT] Conectando ao broker %s:%d ...\n", MQTT_BROKER, MQTT_PORT);

  bool ok;
  if (strlen(MQTT_USER) > 0) {
    ok = mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD);
  } else {
    ok = mqttClient.connect(clientId.c_str());
  }

  if (ok) {
    mqttEverConnected = true;
    Serial.println("[MQTT] Conectado!");
  } else {
    Serial.printf("[MQTT] Falha (rc=%d). Tentará novamente em 5s.\n", mqttClient.state());
  }
}

// ── Publicação MQTT no Fiware ─────────────────────────────────
// Monta o payload em JSON e publica no tópico do IoT Agent.
// O IoT Agent recebe, interpreta e atualiza a entidade no Orion.
//
// Atributos publicados:
//   peso       → peso atual na dock (gramas)
//   consumo    → total consumido no dia (ml)
//   pct        → percentual da meta diária (0-100)
//   fatias     → fatias da meta atingidas (0-4)
//   estado_led → cor atual do LED de feedback
void publishMQTT(float pesoAtualG, const char* estadoLed) {
  if (!mqttClient.connected()) {
    connectMQTT();
    if (!mqttClient.connected()) return;
  }

  float pct = (totalConsumedG / DAILY_GOAL_ML) * 100.0f;
  if (pct > 100.0f) pct = 100.0f;

  // Payload em JSON — formato exigido pelo iotagent-json
  char payload[200];
  snprintf(payload, sizeof(payload),
    "{\"peso\":%.1f,\"consumo\":%.1f,\"pct\":%.0f,\"fatias\":%d,\"estado_led\":\"%s\"}",
    pesoAtualG,
    totalConsumedG,
    pct,
    goalsReached,
    estadoLed
  );

  // Tópico padrão do IoT Agent JSON: /json/<apikey>/<device_id>/attrs
  char topic[100];
  snprintf(topic, sizeof(topic), "/json/%s/%s/attrs", FIWARE_APIKEY, DEVICE_ID);

  bool ok = mqttClient.publish(topic, payload);
  Serial.printf("[MQTT] Publicado em '%s': %s → %s\n",
                topic, payload, ok ? "OK" : "FALHOU");
}

// ── Leitura de peso com média móvel ──────────────────────────
// Lê da HX711 (já em gramas após calibração+tara) e suaviza
// pequenas variações com média móvel circular.
//
// IMPORTANTE: a HX711 só fornece nova amostra a ~10Hz (a cada ~100ms),
// então is_ready() retorna false na maioria das chamadas do loop.
// Quando isso acontece, repetimos a ÚLTIMA leitura crua válida —
// nunca lastStableG, que é uma referência antiga e causaria oscilação
// entre o valor real e o valor antigo.
float readWeightG() {
  float grams;
  if (scale.is_ready()) {
    grams = scale.get_units(1);
    lastRawG = grams;   // atualiza cache
  } else {
    grams = lastRawG;   // repete última leitura válida
  }
  if (grams < 0) grams = 0;
  if (grams > MAX_WEIGHT_G) grams = MAX_WEIGHT_G;

  readSum -= readings[readIndex];
  readings[readIndex] = grams;
  readSum += grams;
  readIndex = (readIndex + 1) % STABLE_READS;

  return readSum / STABLE_READS;
}

// ── Tara ──────────────────────────────────────────────────────
// Usa a tara nativa da HX711 (média de N leituras define o zero)
void doTare() {
  Serial.println("=== TARANDO... aguarde ===");
  scale.tare(20);

  tareOffset      = 0;
  lastStableG     = 0;
  lastRawG        = 0;   // limpa cache pra não pegar valor pré-tara
  stableCandidate = 0;
  waitingStable   = false;
  isAirborne      = false;
  taredOnce       = true;

  // Limpa buffer da média móvel para não arrastar valores antigos
  memset(readings, 0, sizeof(readings));
  readSum   = 0;
  readIndex = 0;

  Serial.println("=== TARA REALIZADA ===");
}

// ── Verificação de metas ──────────────────────────────────────
// Divide a meta diária em 4 fatias e celebra cada uma atingida
void checkGoals() {
  float mlPerStep = (float)DAILY_GOAL_ML / GOAL_STEPS;
  int stepsNow = (int)(totalConsumedG / mlPerStep);
  if (stepsNow > GOAL_STEPS) stepsNow = GOAL_STEPS;

  if (stepsNow > goalsReached) {
    goalsReached = stepsNow;
    celebrateEnd = millis() + STEP_CELEBRATE_MS;

    if (goalsReached >= GOAL_STEPS) {
      Serial.println("🏆 META DIÁRIA ATINGIDA! Parabéns!");
    } else {
      Serial.printf("✅ Fatia %d/%d atingida! (%.0f/%.0f ml)\n",
        goalsReached, GOAL_STEPS, totalConsumedG, (float)DAILY_GOAL_ML);
    }
  }
}

// ── Feedback visual pelo LED RGB ─────────────────────────────
// Retorna o nome da cor atual para incluir no payload MQTT
const char* updateLED(unsigned long now, float currentG) {
  if (taringNow)               { ledBlue();   return "azul"; }
  if (now < celebrateEnd) {
    if (goalsReached >= GOAL_STEPS) {
      bool pulse = ((now / 400) % 2 == 0);
      pulse ? ledGreen() : ledOff();
    } else { ledGreen(); }
    return "verde";
  }
  if (isAirborne) {
    bool blink = ((now / 300) % 2 == 0);
    blink ? ledYellow() : ledOff();
    return "amarelo";
  }
  if (lastDrinkMs > 0 && (now - lastDrinkMs) >= IDLE_ALERT_MS) {
    bool blink = ((now / 600) % 2 == 0);
    blink ? ledRed() : ledOff();
    return "vermelho";
  }
  if (waitingStable) {
    bool blink = ((now / 200) % 2 == 0);
    blink ? setRGB(0, 0, 80) : ledOff();
    return "azul_fraco";
  }
  float pct = totalConsumedG / DAILY_GOAL_ML;
  if (pct > 1.0f) pct = 1.0f;
  if (pct < 0.01f) { ledOff(); return "apagado"; }
  uint8_t brightness = (uint8_t)(pct * 180);
  setRGB(0, brightness, 0);
  return "verde";
}

// ── Log serial periódico ──────────────────────────────────────
void logSerial(unsigned long now, float currentG) {
  if ((now - lastLogMs) < LOG_INTERVAL_MS) return;
  lastLogMs = now;

  float pct = (totalConsumedG / DAILY_GOAL_ML) * 100.0f;
  if (pct > 100.0f) pct = 100.0f;
  unsigned long idleSec = (lastDrinkMs > 0) ? (now - lastDrinkMs) / 1000 : 0;

  Serial.printf("[%6lu ms] Peso: %6.1f g | Tara: %5.1f g | "
                "Consumido: %6.1f ml (%.0f%%) | "
                "Sem beber: %lus | Fatias: %d/%d%s\n",
    now, currentG, tareOffset,
    totalConsumedG, pct, idleSec,
    goalsReached, GOAL_STEPS,
    isAirborne ? " | ⚠️ NO AR" : "");
}

// ── Setup ─────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("\n\n=== WayCare Dock — Simulação Wokwi + MQTT/Fiware ===");
  Serial.println("  Segure o botão por 2s para tarar.");
  Serial.println("  Ajuste a célula de carga para simular peso da água.\n");

  // Configura PWM para o LED RGB
  ledcAttach(PIN_R, PWM_FREQ, PWM_RES);
  ledcAttach(PIN_G, PWM_FREQ, PWM_RES);
  ledcAttach(PIN_B, PWM_FREQ, PWM_RES);
  pinMode(PIN_BTN, INPUT_PULLUP);

  // Inicializa HX711 + célula de carga
  scale.begin(PIN_HX_DT, PIN_HX_SCK);
  scale.set_scale(HX_SCALE_FACTOR);

  Serial.println("[HX711] Aguardando célula de carga...");
  unsigned long t0 = millis();
  while (!scale.is_ready() && (millis() - t0) < 3000) {
    delay(100);
  }
  if (scale.is_ready()) {
    Serial.println("[HX711] Pronta!");
  } else {
    Serial.println("[HX711] Timeout — verifique fiação no diagram.json.");
  }

  memset(readings, 0, sizeof(readings));
  delay(500);
  doTare();
  lastDrinkMs = 0;

  // Conecta ao Wi-Fi e ao broker MQTT da VM
  connectWiFi();
  connectMQTT();
}

// ── Loop principal ────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  // Mantém a conexão MQTT ativa e processa mensagens recebidas
  if (mqttClient.connected()) {
    mqttClient.loop();
  } else if (WiFi.status() == WL_CONNECTED && mqttEverConnected) {
    connectMQTT();
  }

  // ── 1. Botão de tara ─────────────────────────────────────────
  bool btnPressed = (digitalRead(PIN_BTN) == LOW);
  if (btnPressed && !btnWasPressed) {
    btnPressStart = now; btnWasPressed = true; taringNow = false;
    Serial.println("Botao pressionado - segure 2s para tarar...");
  }
  if (btnPressed && btnWasPressed && !taringNow) {
    unsigned long held = now - btnPressStart;
    bool blinkBtn = ((held / 200) % 2 == 0);
    blinkBtn ? ledBlue() : ledOff();
    if (held >= TARE_HOLD_MS) {
      taringNow = true; ledBlue(); doTare();
      for (int i = 0; i < 3; i++) { ledOff(); delay(150); ledBlue(); delay(150); }
      taringNow = false; isAirborne = false; waitingStable = false;
    }
  }
  if (!btnPressed && btnWasPressed) {
    btnWasPressed = false; taringNow = false;
    if ((now - btnPressStart) < TARE_HOLD_MS)
      Serial.println("Botao solto antes de 2s - tara cancelada.");
  }

  // ── 2. Leitura de peso ────────────────────────────────────────
  float currentG = readWeightG();

  // ── 3. Detecção de garrafa no ar ──────────────────────────────
  // Quando o peso cai abaixo do threshold, a garrafa foi levantada
  if (taredOnce && !isAirborne && currentG < AIRBORNE_THRESHOLD) {
    isAirborne = true; airborneStart = now;
    Serial.println("⚠️  Garrafa levantada — aguardando retorno à dock...");
  }
  if (isAirborne) {
    if (currentG >= AIRBORNE_THRESHOLD) {
      isAirborne = false;
      unsigned long airDuration = now - airborneStart;
      waitingStable = false;
      Serial.printf("🔄 Garrafa retornou à dock após %lu ms. "
                    "Ref. anterior: %.1f g → atual: %.1f g. Verificando...\n",
                    airDuration, lastStableG, currentG);
    }
    const char* cor = updateLED(now, currentG);
    logSerial(now, currentG);
    return;
  }

  // ── 4. Detecção de variação estável ───────────────────────────
  // Aguarda STABLE_TIME_MS para confirmar que o peso se estabilizou
  // antes de registrar consumo ou abastecimento
  float delta = lastStableG - currentG;
  if (abs(delta) >= MIN_DELTA_G) {
    if (!waitingStable || abs(currentG - stableCandidate) > MIN_DELTA_G) {
      stableCandidate = currentG; stableStart = now; waitingStable = true;
    }
  }
  if (waitingStable) {
    if (abs(currentG - stableCandidate) > MIN_DELTA_G) {
      stableCandidate = currentG; stableStart = now;
    }
    if ((now - stableStart) >= STABLE_TIME_MS) {
      float confirmedDelta = lastStableG - currentG;

      if (confirmedDelta >= MIN_DELTA_G) {
        // Peso diminuiu → consumo confirmado
        totalConsumedG += confirmedDelta;
        lastDrinkMs     = now;

        Serial.println("─────────────────────────────────");
        Serial.printf("💧 CONSUMO CONFIRMADO: %.1f ml\n", confirmedDelta);
        Serial.printf("   Total do dia: %.1f / %d ml\n", totalConsumedG, DAILY_GOAL_ML);
        Serial.printf("   Ref. anterior: %.1f g → atual: %.1f g\n", lastStableG, currentG);
        Serial.println("─────────────────────────────────");
        checkGoals();

        // Publica dados atualizados no Fiware
        publishMQTT(currentG, "verde");

      } else if (confirmedDelta < -MIN_DELTA_G) {
        // Peso aumentou → garrafa foi abastecida
        Serial.println("─────────────────────────────────");
        Serial.printf("🚰 ABASTECIMENTO CONFIRMADO: +%.1f g\n", -confirmedDelta);
        Serial.printf("   Ref. anterior: %.1f g → atual: %.1f g\n", lastStableG, currentG);
        Serial.println("─────────────────────────────────");

        // Publica estado atualizado no Fiware
        publishMQTT(currentG, "azul");
      }

      lastStableG = currentG; waitingStable = false;
    }
  }

  // ── 5. Feedback visual ────────────────────────────────────────
  const char* corAtual = updateLED(now, currentG);

  // ── 6. Log serial ─────────────────────────────────────────────
  logSerial(now, currentG);

  // ── 7. Heartbeat MQTT ─────────────────────────────────────────
  // Publica o estado atual periodicamente mesmo sem novo consumo
  if ((now - lastMqttReportMs) >= MQTT_REPORT_MS) {
    lastMqttReportMs = now;
    publishMQTT(currentG, corAtual);
  }
}
