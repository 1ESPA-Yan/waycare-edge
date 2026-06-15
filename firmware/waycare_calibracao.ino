/*
 * ============================================================================
 *  WayCare Dock  -  SKETCH DE CALIBRAÇÃO (rodar UMA vez)
 * ============================================================================
 *  Objetivo: descobrir o HX_SCALE_FACTOR da SUA célula de carga.
 *  Não usa WiFi nem MQTT — só HX711 + Serial Monitor.
 *
 *  COMO USAR:
 *    1. Suba este sketch na ESP32.
 *    2. Abra o Serial Monitor a 115200 baud.
 *    3. Siga as instruções que aparecem na tela:
 *         - deixe a dock VAZIA quando pedir a tara
 *         - coloque um peso CONHECIDO quando pedir (ex: 1000 g)
 *    4. Anote o FATOR que ele imprime no final.
 *    5. Cole esse número no firmware definitivo (waycare_dock_real.ino),
 *       na linha #define HX_SCALE_FACTOR.
 *
 *  Depois disso, este sketch não é mais necessário.
 * ============================================================================
 */

#include "HX711.h"

#define HX_DT   16
#define HX_SCK  17

// >>> TROQUE pelo peso do objeto de referência que você vai usar (em gramas) <<<
#define PESO_CONHECIDO_G   1000.0f

HX711 scale;

void esperarEnter() {
  while (Serial.available()) Serial.read();      // limpa buffer
  while (!Serial.available()) delay(50);         // espera digitar algo + Enter
  while (Serial.available()) Serial.read();      // limpa de novo
}

void setup() {
  Serial.begin(115200);
  delay(500);
  scale.begin(HX_DT, HX_SCK);

  Serial.println("\n=== CALIBRACAO WAYCARE DOCK ===");
  Serial.println("Deixe a dock VAZIA (sem nada em cima).");
  Serial.println("Quando estiver vazia, digite qualquer tecla e Enter.");
  esperarEnter();

  Serial.println("Tarando (zerando)...");
  scale.set_scale();                 // sem fator ainda
  scale.tare(20);                    // zera com a dock vazia
  Serial.println("Zerado.");

  Serial.print("Agora coloque o peso conhecido de ");
  Serial.print(PESO_CONHECIDO_G);
  Serial.println(" g na dock.");
  Serial.println("Quando estiver estavel, digite qualquer tecla e Enter.");
  esperarEnter();

  // Lê o valor bruto médio com o peso conhecido em cima
  long bruto = scale.get_value(20);  // média de 20 leituras (já descontada a tara)
  float fator = bruto / PESO_CONHECIDO_G;

  Serial.println("\n========================================");
  Serial.print("Valor bruto medido: ");
  Serial.println(bruto);
  Serial.print(">>> SEU HX_SCALE_FACTOR = ");
  Serial.println(fator, 4);
  Serial.println("========================================");
  Serial.println("Copie esse numero para o firmware definitivo.");
  Serial.println("\n--- Teste: leituras em gramas agora ---");

  scale.set_scale(fator);            // aplica o fator descoberto
}

void loop() {
  // Mostra o peso em gramas pra você conferir se a calibração ficou boa.
  // Ponha/tire pesos e veja se os gramas batem com a realidade.
  Serial.print("Peso atual: ");
  Serial.print(scale.get_units(5), 1);
  Serial.println(" g");
  delay(800);
}
