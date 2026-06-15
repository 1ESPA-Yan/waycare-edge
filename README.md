# 💧 WayCare Dock — Edge Computing & IoT
### Challenge Care Plus | FIAP — 1º ano Engenharia de Software | Sprint 3

---

## 📋 Descrição do Projeto

O **WayCare Dock** é um suporte inteligente para garrafa d'água que monitora o
consumo de hidratação do usuário em tempo real. O ESP32 lê uma célula de carga e
publica o peso via MQTT; um **backend Python na nuvem** calcula o consumo, a meta
diária personalizada e a gamificação, grava o histórico em **SQLite** e serve uma
**API REST** para o site da plataforma WayCare. A tara é acionada pelo site.

### O que mudou nesta sprint (vs. Sprint 2)

- **Arquitetura híbrida edge–nuvem:** o ESP32 deixou de calcular consumo. Agora ele
  só estabiliza e publica o peso; toda a lógica de negócio (consumo, %, fatias,
  streak, estado do LED) roda no backend Python.
- **Backend Python (Flask + MQTT + SQLite):** lê o peso do Orion, processa, grava
  histórico e expõe a API REST consumida pelo site.
- **SQLite no lugar do MongoDB** para o histórico que o site consome (o MongoDB
  segue como base interna do FIWARE).
- **Tara pelo site:** o botão físico foi removido; a tara é um comando enviado do
  site ao ESP32 via MQTT.
- **Meta personalizada:** calculada por peso, sexo, nível de atividade física
  (ACSM) e temperatura local (Open-Meteo), com base científica documentada.
- **Detecção de garrafa fora:** a tara é feita com a garrafa vazia na dock, então
  peso negativo = garrafa removida.
- **Deploy com HTTPS** (nginx + DuckDNS + Let's Encrypt) para o site consumir a API.

---

## 🏗️ Arquitetura

```
ESP32 --peso(MQTT)--> IoT Agent --> Orion --HTTP--> Python --> SQLite --> Site
  ^                                                   |
  +------------- comando tara/led (MQTT) -------------+
                                         Open-Meteo (temperatura da cidade)
```

![Diagrama de Arquitetura WayCare](docs/WayCare_Arquitetura_v99.drawio.png)

- **Edge (ESP32):** estabiliza o peso (mediana da janela), publica so na mudanca,
  executa tara/LED. Nao calcula consumo.
- **Nuvem (Python/Flask):** le o peso do Orion, maquina de estado do consumo,
  gamificacao, meta personalizada, historico em SQLite, API REST e comandos MQTT.
- **FIWARE (Docker):** Orion + IoT Agent JSON + Mosquitto + MongoDB.

---

## 📁 Estrutura do Repositorio

```
waycare-edge/
├── README.md
├── DEPLOY.md                    <- passo a passo completo de instalacao
│
├── firmware/
│   ├── waycare_dock_wokwi.ino   <- versao para simulacao no Wokwi
│   ├── waycare_dock_real.ino    <- versao para o hardware real (calibrado)
│   ├── waycare_calibracao.ino   <- sketch de calibracao (rodar 1x)
│   ├── diagram.json             <- circuito do Wokwi
│   └── libraries.txt
│
├── backend/
│   ├── waycare_python.py        <- Flask + MQTT + SQLite + logica de consumo/meta
│   ├── requirements.txt
│   └── waycare-python.service   <- servico systemd
│
├── fiware/
│   └── docker-compose.yml       <- Orion + IoT Agent + Mosquitto + MongoDB
│
├── infra/
│   └── waycare-nginx.conf       <- proxy reverso HTTPS (nginx + DuckDNS)
│
├── postman/
│   ├── WayCare_Fiware_Sprint3.postman_collection.json   <- provisionamento atual
│   └── WayCare_Fiware_Sprint2.postman_collection.json   <- historico
│
└── docs/
    ├── WayCare_API.md           <- contrato da API REST (para o front-end)
    └── WayCare_Arquitetura_v99.drawio.png
```

> O **site (front-end)** fica em repositorio separado e consome a API descrita em
> `docs/WayCare_API.md`.

---

## 🔌 Como Executar

Passo a passo detalhado em **DEPLOY.md**. Resumo:

1. **FIWARE:** `cd fiware && docker compose up -d`
2. **Provisionar:** importar `postman/WayCare_Fiware_Sprint3...json` e rodar (1x).
3. **Backend:** `pip3 install -r backend/requirements.txt --break-system-packages`
   e instalar o servico systemd.
4. **HTTPS:** nginx + DuckDNS + certbot (`infra/waycare-nginx.conf`).
5. **Firmware:** calibrar (`waycare_calibracao.ino`), colar o fator no
   `waycare_dock_real.ino`, preencher WiFi e subir. (Ou usar a versao Wokwi.)
6. **Perfil:** definir peso/sexo/atividade/cidade (define a meta).

---

## 🧪 Base cientifica da meta

- **Base por peso/sexo:** ml/kg clinico (35 m / 31 f), ordem de grandeza da EFSA.
- **Temperatura:** ajuste acima de 25C, com teto (estimativa transparente).
- **Atividade:** ACSM (0,4-0,8 L/h de exercicio) -> leve +350, moderado +700,
  intenso +1200 ml.
- E uma **estimativa**, nao prescricao medica.

---

## 👥 Integrantes

| Nome | RM |
|------|----|
| Joao Victor Melo | 566640 |
| Yan Lucas | 567046 |
| Gustavo Hiruo | 567625 |
| Gustavo Macedo | 567594 |
