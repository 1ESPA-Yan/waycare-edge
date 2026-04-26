# 💧 WayCare Dock — Edge Computing & IoT
### Challenge Care Plus | FIAP — 1º ano Engenharia de Software | Sprint 2

---

## 📋 Descrição do Projeto

O **WayCare Dock** é um suporte inteligente para garrafa d'água que monitora automaticamente o consumo de hidratação do usuário. Utilizando um ESP32 conectado via MQTT a uma plataforma FIWARE na nuvem, o dispositivo captura dados em tempo real e os disponibiliza para consulta, compondo a camada de Edge Computing do ecossistema WayCare da Care Plus.

Nesta sprint, o hardware foi simulado digitalmente no **Wokwi**, com o potenciômetro simulando uma célula de carga (balança) e o LED RGB fornecendo feedback visual ao usuário.

---

## 🏗️ Arquitetura da Solução

```
[Edge Layer]          [Connectivity]     [Backend]              [Application]
ESP32 (Wokwi)   →    MQTT :1883     →   IoT Agent :4041   →   Orion :1026
Potenciômetro         Mosquitto          FIWARE                 Context Broker
LED RGB               (Broker)           (Tradutor)             (Estado atual)
Pushbutton
```

> O diagrama completo da arquitetura está na pasta `/docs/diagrama_arquitetura.png`

---

## ⚙️ Tecnologias Utilizadas

| Camada | Tecnologia |
|--------|-----------|
| Hardware simulado | ESP32 DevKit C v4 (Wokwi) |
| Firmware | C++ (Arduino Framework) |
| Protocolo IoT | MQTT (PubSubClient) |
| Broker MQTT | Eclipse Mosquitto 1.6.14 |
| IoT Agent | FIWARE IoT Agent JSON |
| Context Broker | FIWARE Orion |
| Banco de dados | MongoDB 4.4 |
| Infraestrutura | Docker + Docker Compose |
| Cloud | Microsoft Azure (VM Ubuntu) |

---

## 📁 Estrutura do Repositório

```
waycare-edge/
│
├── firmware/
│   └── waycare_dock_mqtt.ino       # Código-fonte do ESP32
│
├── fiware/
│   └── docker-compose.yml          # Stack completo do FIWARE
│
├── postman/
│   └── WayCare_Fiware_Sprint2.postman_collection.json
│
├── docs/
│   └── diagrama_arquitetura.png    # Diagrama das camadas
│
└── README.md
```

---

## 🔌 Como Executar

### Pré-requisitos

- Conta no [Wokwi](https://wokwi.com)
- VM Linux com Docker instalado e portas abertas: `22`, `1026`, `1883`, `4041`

---

### 1. Subir o FIWARE na VM

Acesse a VM via SSH e execute:

```bash
cd waycare-fiware
docker compose up -d
```

Verifique se os 4 containers estão rodando:

```bash
docker ps
```

Os containers esperados são: `fiware-orion`, `fiware-iot-agent`, `mosquitto`, `db-mongo`.

---

### 2. Configurar o FIWARE via Postman

Importe o arquivo `postman/WayCare_Fiware_Sprint2.postman_collection.json` no Postman.

Execute os requests nessa ordem:

| Ordem | Request | Resultado esperado |
|-------|---------|-------------------|
| 1 | `Orion → 1. Version` | 200 OK |
| 2 | `IOT Agent → 1.1 Health Check` | 200 OK |
| 3 | `IOT Agent → 2. Provisioning Service Group` | 201 Created |
| 4 | `IOT Agent → 3. Provisioning WayCare Dock Device` | 201 Created |

---

### 3. Executar a Simulação no Wokwi

Acesse o link da simulação pública: **[INSERIR LINK DO WOKWI AQUI]**

- Gire o **potenciômetro** para simular o peso da água na garrafa
- Aguarde `💧 CONSUMO CONFIRMADO` aparecer no Serial Monitor
- O ESP32 publicará os dados automaticamente via MQTT

---

### 4. Verificar os dados no Orion

No Postman, execute:

```
Orion → 2. Get all entities
```

Resposta esperada:

```json
{
  "id": "WaycareDock:dock01",
  "type": "WaycareDock",
  "peso": { "value": 1155.6 },
  "consumo": { "value": 608.1 },
  "pct": { "value": 30 },
  "fatias": { "value": 1 },
  "estado_led": { "value": "verde" }
}
```

---

## 📡 Tópico MQTT

| Campo | Valor |
|-------|-------|
| Broker | `IP_DA_VM:1883` |
| Tópico | `/json/waycare2025/dock01/attrs` |
| Formato | JSON |
| API Key | `waycare2025` |
| Device ID | `dock01` |

---

## 🎯 Atributos Monitorados

| Atributo | Tipo | Descrição |
|----------|------|-----------|
| `peso` | Float | Peso atual na dock (gramas) |
| `consumo` | Float | Total consumido no dia (ml) |
| `pct` | Float | Percentual da meta diária (%) |
| `fatias` | Integer | Fatias da meta atingidas (0-4) |
| `estado_led` | Text | Cor atual do LED de feedback |

---

## 🎥 Vídeo de Demonstração

**[INSERIR LINK DO YOUTUBE AQUI]**

---

## 👥 Integrantes

| Nome | RM |
|------|----|
| João Victor Melo | 566640 |
| Yan Lucas | 567046 |
| Gustavo Hiruo | 567625 |
| Gustavo Macedo | 567594 |

---

## 🔗 Links

- 🔵 [Simulação Wokwi](INSERIR LINK)
- 📺 [Vídeo YouTube](INSERIR LINK)
- 📋 [Challenge Care Plus — FIAP](https://www.fiap.com.br)
