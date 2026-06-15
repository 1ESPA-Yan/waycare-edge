# WayCare Dock — Passo a passo completo (Sprint 3)

Arquitetura híbrida: o ESP32 só estabiliza e publica o peso; o Python na nuvem
calcula consumo / % / fatias / streak / LED e a **meta personalizada** (peso +
gênero + temperatura local via Open-Meteo), grava em SQLite e serve a API pro
React. Tara e perfil vêm do site.

```
ESP32 ──peso(MQTT)──> IoT Agent ──> Orion ──HTTP──> Python ──> SQLite ──> React
  ^                                                    │
  └──────────── comando tara/led (MQTT) ───────────────┘
                                                      Open-Meteo (temperatura)
```

Você está na **VM da WayCare**, que já tem os 4 containers FIWARE rodando.

---

## 1. Resetar o FIWARE (limpa o provisionamento antigo)

O `db-mongo` não tem volume, então derrubar os containers apaga o estado do
Orion/IoT-Agent. É o jeito limpo de recomeçar sem caçar pastas antigas.

```bash
cd ~/waycare-fiware
docker compose down
docker compose up -d
docker ps          # confira os 4 containers
```

## 2. Re-provisionar no Postman

Importe `WayCare_Fiware_Sprint3.postman_collection.json`. Troque `SEU_IP_DA_VM`
pelo IP público. Rode na ordem:

1. `1.1` e `1.2` → 200 OK
2. `2.1 Criar Service Group` → 201
3. `2.2 Registrar Dispositivo` → 201

> O device tem **um único atributo: `peso`**. O resto é calculado no backend.

## 3. Subir o backend Python

```bash
mkdir -p ~/waycare-backend && cd ~/waycare-backend
# copie waycare_python.py e requirements.txt pra cá

pip3 install -r requirements.txt --break-system-packages
python3 waycare_python.py        # teste em primeiro plano
```

Em outro terminal:

```bash
curl http://localhost:5000/status
```

Funcionou? Suba como serviço:

```bash
sudo cp waycare-python.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now waycare-python
sudo systemctl status waycare-python
journalctl -u waycare-python -f   # logs (goles aparecem aqui)
```

> **Abra a porta 5000** no Network Security Group da Azure (e `sudo ufw allow 5000` se usar ufw).

## 4. Expor o backend em HTTPS (necessário pro deploy do site)

Site publicado em HTTPS não chama backend `http://` (mixed content). Mesmo
esquema do DarkSky: nginx + DuckDNS + Let's Encrypt.

```bash
# DuckDNS: crie um subdomínio NOVO (ex: waycare-fiap) apontando pro IP da VM.
sudo apt update && sudo apt install -y nginx certbot python3-certbot-nginx

# copie waycare-nginx.conf e ajuste o server_name pro seu domínio
sudo cp waycare-nginx.conf /etc/nginx/sites-available/waycare
sudo ln -s /etc/nginx/sites-available/waycare /etc/nginx/sites-enabled/
sudo nginx -t && sudo systemctl reload nginx

sudo certbot --nginx -d waycare-fiap.duckdns.org
```

Abra as portas **80 e 443** na Azure. Depois disso o backend responde em
`https://waycare-fiap.duckdns.org/status`. Esse é o endereço que o site usa
(local e produção).

## 5. Atualizar o firmware no Wokwi

1. Cole `waycare_dock.ino` no Wokwi (substitui o antigo).
2. Troque `SEU_IP_DA_VM` pelo IP da VM.
3. `libraries.txt`: `PubSubClient` e `HX711`.
4. Rode. Arraste o slider da célula de carga, espere ~3s parado: aparece
   `Publicado -> {"peso":...}` no Serial Monitor.
5. Confira no Postman `3.2` que o `peso` chegou no Orion.
6. No `journalctl`, baixe o peso e veja `GOLE: ... ml`.

## 6. Configurar o perfil (define a meta personalizada)

Faça uma vez (ou deixe o site fazer pela página de perfil):

```bash
curl -X POST https://waycare-fiap.duckdns.org/perfil \
  -H "Content-Type: application/json" \
  -d '{"peso":78,"altura":180,"genero":"m","cidade":"São Paulo"}'
# -> {"ok":true,"meta":2790,"temp":24.3,"cidade":"São Paulo"}
```

Sem perfil, a meta fica no padrão de 2500ml.

## 7. Ligar o React

Copie a pasta `waycare-react/` pro projeto. Crie um `.env` na raiz:

```
VITE_API_URL=https://waycare-fiap.duckdns.org
```

Na Vercel, ponha a mesma variável em Settings > Environment Variables. Use o
hook `useWayCareDock()`, o `<TaraButton/>` e a página `PerfilHidratacao`.

## 8. Testar o fluxo completo

| Ação | Esperado |
|---|---|
| Garrafa cheia, estabiliza | `peso` aparece no `/status` |
| Baixar 100g | `consumo_dia` +100ml, gráfico atualiza |
| Subir o peso | tratado como refill, consumo NÃO cai |
| Botão "Tarar" no site | Wokwi fica azul, zera, consumo sem salto falso |
| Salvar perfil no site | `meta` muda conforme peso/gênero/temperatura |
| 10 min sem gole | LED vermelho |
| Atingir a meta | LED verde pulsando |
