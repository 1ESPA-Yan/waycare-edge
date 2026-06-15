#!/usr/bin/env python3
# ============================================================================
#  WayCare Dock  -  Backend de processamento na nuvem (VM Azure)
# ============================================================================
#  RESPONSABILIDADES (toda a lógica de negócio mora aqui, não no ESP32):
#    1) Lê o peso estável do Orion Context Broker (HTTP, padrão FIWARE)
#    2) Máquina de estado: distingue GOLE x REABASTECIMENTO x GARRAFA REMOVIDA
#    3) Acumula consumo do dia, calcula %, fatias, streak e estado do LED
#    4) Grava histórico em SQLite (React lê daqui, sem MongoDB)
#    5) Expõe API REST: GET /status, GET /history, POST /tara
#    6) Envia comandos de volta ao ESP32 via MQTT (tara e cor do LED)
# ============================================================================

import sqlite3
import json
import time
import threading
from datetime import datetime, date

import requests
import paho.mqtt.client as mqtt
from flask import Flask, jsonify, request
from flask_cors import CORS

# ----------------------------------------------------------------------------
# CONFIGURAÇÃO
# ----------------------------------------------------------------------------
ORION       = "http://localhost:1026"          # Python roda na mesma VM
MQTT_HOST   = "localhost"
MQTT_PORT   = 1883

FIWARE_SVC  = "waycare"                         # fiware-service header
FIWARE_PATH = "/"
ENTITY_ID   = "WaycareDock:dock01"              # entidade no Orion
TOPIC_CMD   = "waycare/dock01/cmd"              # tópico de comando p/ o ESP32

DB_PATH     = "waycare.db"

META_PADRAO    = 2500.0   # meta usada enquanto não há perfil configurado (ml)
MIN_DELTA      = 20.0     # variação mínima (ml) p/ contar um evento
# A tara é feita com a GARRAFA VAZIA na dock. Então o zero = garrafa vazia.
# Remover a garrafa faz o peso ficar NEGATIVO -> sinal de "garrafa fora".
LIMIAR_FORA    = -20.0    # peso < -20g = garrafa removida da dock
INATIVIDADE_S  = 60 * 60  # 1 hora sem beber (com garrafa presente) -> LED vermelho
POLL_S         = 2        # intervalo de leitura do Orion
SNAPSHOT_S     = 60       # intervalo entre snapshots no histórico
REENVIO_LED_S  = 15       # reenvia a cor do LED ao ESP32 a cada 15s (anti-azul-travado)
REBASELINE_S   = 8        # janela (s) de re-baseline após a tara (cobre latência do ESP32)

# --- Parâmetros da fórmula de meta personalizada ---------------------------
# Aproximação transparente (NÃO é prescrição médica). Bases científicas:
#  - Base por peso/sexo: ml/kg clínico (~30-35 ml/kg), na ordem de grandeza da
#    EFSA (2,5 L homem / 2,0 L mulher, já em temperatura e atividade MODERADAS).
#  - Temperatura: a EFSA assume clima moderado; ajuste só acima de ~25°C (topo
#    da faixa moderada). NÃO há constante científica de "ml por grau" — o efeito
#    depende de umidade/sol/aclimatação. Por isso é percentual pequeno COM TETO,
#    rotulado como estimativa.
#  - Atividade: a base já assume atividade moderada; adicional ancorado na ACSM
#    (0,4-0,8 L por hora de exercício intenso), convertido em médias diárias.
ML_POR_KG_M    = 35.0     # ml por kg (referência masculina)
ML_POR_KG_F    = 31.0     # ml por kg (referência feminina)
TEMP_BASE      = 25.0     # °C: acima disso começa o ajuste por calor
PCT_POR_GRAU   = 0.025    # +2,5% da base por °C acima de TEMP_BASE
TEMP_ADD_MAX   = 500.0    # teto do ajuste de temperatura (ml) p/ pessoa não-atleta
ATIVIDADE_ADD  = {        # ml/dia adicionais por nível (derivado da ACSM)
    "sedentario": 0,      # pouca ou nenhuma atividade
    "leve":       350,    # exercício leve poucas vezes/semana
    "moderado":   700,    # ~1h de exercício quase todo dia
    "intenso":    1200,   # treino intenso/longo ou atleta
}
META_MIN       = 1500.0   # piso de segurança (ml)
META_MAX       = 5000.0   # teto de segurança (ml)

# ----------------------------------------------------------------------------
# ESTADO GLOBAL (protegido por lock pois Flask e o loop rodam em threads)
# ----------------------------------------------------------------------------
lock = threading.Lock()

estado = {
    "peso": 0.0,             # último peso estável lido
    "peso_ref": None,        # referência p/ calcular delta
    "consumo_dia": 0.0,      # ml acumulados hoje
    "pct": 0.0,
    "fatias": 0,
    "estado_led": "verde",
    "streak": 0,
    "ultima_atividade": time.time(),
    "rebaseline_ate": 0.0,     # re-baseline ativo até este timestamp (pós-tara)
    "garrafa_fora": False,
    "data_dia": date.today().isoformat(),
    "online": False,
    "ultimo_pacote": 0.0,    # timestamp da última leitura válida
    "meta": META_PADRAO,     # meta atual (personalizada pelo perfil)
    "perfil": None,          # dados do dono da dock (peso/altura/genero/cidade)
    "temp": None,            # temperatura local mais recente (°C)
    "hibernado": False,      # quando True, a dock não processa consumo
    "ultimo_envio_led": 0.0, # timestamp do último comando de LED enviado
}

mqtt_client = mqtt.Client()

# ============================================================================
#  BANCO DE DADOS (SQLite)
# ============================================================================
def init_db():
    """Cria as tabelas se não existirem."""
    con = sqlite3.connect(DB_PATH)
    con.execute("""CREATE TABLE IF NOT EXISTS snapshots (
        ts REAL, consumo REAL, pct REAL, peso REAL)""")
    con.execute("""CREATE TABLE IF NOT EXISTS eventos (
        ts REAL, tipo TEXT, valor REAL)""")
    con.execute("""CREATE TABLE IF NOT EXISTS dias (
        data TEXT PRIMARY KEY, consumo REAL, meta_batida INTEGER)""")
    con.execute("""CREATE TABLE IF NOT EXISTS perfil (
        id INTEGER PRIMARY KEY CHECK (id = 1),
        peso REAL, atividade TEXT, genero TEXT,
        cidade TEXT, lat REAL, lon REAL,
        meta REAL, temp REAL, atualizado_em REAL)""")
    con.commit()
    con.close()

def db_evento(tipo, valor):
    con = sqlite3.connect(DB_PATH)
    con.execute("INSERT INTO eventos VALUES (?,?,?)", (time.time(), tipo, valor))
    con.commit(); con.close()

def db_snapshot():
    con = sqlite3.connect(DB_PATH)
    con.execute("INSERT INTO snapshots VALUES (?,?,?,?)",
                (time.time(), estado["consumo_dia"], estado["pct"], estado["peso"]))
    con.commit(); con.close()

def db_fechar_dia(data_str, consumo, meta):
    """Salva o total do dia e recalcula o streak."""
    bateu = 1 if consumo >= meta else 0
    con = sqlite3.connect(DB_PATH)
    con.execute("INSERT OR REPLACE INTO dias VALUES (?,?,?)",
                (data_str, consumo, bateu))
    con.commit(); con.close()

def calcular_streak():
    """Conta dias consecutivos (até ontem) em que a meta foi batida."""
    con = sqlite3.connect(DB_PATH)
    rows = con.execute(
        "SELECT data, meta_batida FROM dias ORDER BY data DESC").fetchall()
    con.close()
    streak = 0
    for _, bateu in rows:
        if bateu:
            streak += 1
        else:
            break
    return streak

# ============================================================================
#  PERFIL + META PERSONALIZADA + TEMPERATURA (Open-Meteo)
# ============================================================================
def geocodificar(cidade):
    """Cidade -> (lat, lon, nome). Filtra para aceitar só lugar povoado real
    (feature_code 'PPL...'), rejeitando país/estado digitado por engano."""
    try:
        url = "https://geocoding-api.open-meteo.com/v1/search"
        r = requests.get(url, params={"name": cidade, "count": 5,
                                       "language": "pt", "format": "json"},
                          timeout=4)
        for g in r.json().get("results", []):
            fc = g.get("feature_code", "")
            if fc.startswith("PPL"):       # PPL = populated place (cidade/vila)
                return g["latitude"], g["longitude"], g["name"]
        # nenhum resultado é cidade (ex: usuário digitou um país)
    except Exception as e:
        print("Erro geocoding:", e)
    return None, None, None

def temperatura_atual(lat, lon):
    """Temperatura atual (°C) via Open-Meteo Forecast. None se falhar."""
    try:
        url = "https://api.open-meteo.com/v1/forecast"
        r = requests.get(url, params={"latitude": lat, "longitude": lon,
                                      "current": "temperature_2m"}, timeout=4)
        return float(r.json()["current"]["temperature_2m"])
    except Exception as e:
        print("Erro temperatura:", e)
    return None

def calcular_meta(peso, genero, temp, atividade):
    """Meta diária (ml) e detalhamento. Base por peso/sexo + ajuste de calor
    (acima de 25°C, com teto) + ajuste de atividade (ACSM)."""
    ml_kg = ML_POR_KG_F if str(genero).lower().startswith("f") else ML_POR_KG_M
    base = peso * ml_kg

    ajuste_temp = 0.0
    if temp is not None and temp > TEMP_BASE:
        ajuste_temp = min(base * PCT_POR_GRAU * (temp - TEMP_BASE), TEMP_ADD_MAX)

    ajuste_ativ = float(ATIVIDADE_ADD.get(atividade, 0))

    total = max(META_MIN, min(META_MAX, base + ajuste_temp + ajuste_ativ))
    return {
        "meta": round(total),
        "base": round(base),
        "ajuste_temp": round(ajuste_temp),
        "ajuste_atividade": round(ajuste_ativ),
    }

def aplicar_perfil(p):
    """Recebe dict de perfil, busca temperatura, calcula meta e persiste."""
    nome_in = p.get("cidade", "")
    lat, lon = p.get("lat"), p.get("lon")
    nome = nome_in
    aviso = None
    if (lat is None or lon is None) and nome_in:
        lat, lon, nome_geo = geocodificar(nome_in)
        if lat is None:
            aviso = "Cidade não encontrada — digite uma cidade (não um país/estado)."
            nome = nome_in
        else:
            nome = nome_geo

    temp = temperatura_atual(lat, lon) if lat is not None else None
    atividade = p.get("atividade", "moderado")
    det = calcular_meta(float(p["peso"]), p.get("genero", "m"), temp, atividade)

    perfil = {"peso": float(p["peso"]), "genero": p.get("genero", "m"),
              "atividade": atividade, "cidade": nome,
              "lat": lat, "lon": lon}

    with lock:
        estado["perfil"] = perfil
        estado["temp"] = temp
        estado["meta"] = det["meta"]

    con = sqlite3.connect(DB_PATH)
    con.execute("""INSERT OR REPLACE INTO perfil
        VALUES (1,?,?,?,?,?,?,?,?,?)""",
        (perfil["peso"], atividade, perfil["genero"], perfil["cidade"],
         lat, lon, det["meta"], temp, time.time()))
    con.commit(); con.close()
    return {**det, "temp": temp, "cidade": nome, "aviso": aviso}

def carregar_perfil():
    """Carrega o perfil salvo no boot (se houver)."""
    con = sqlite3.connect(DB_PATH)
    row = con.execute("SELECT peso,atividade,genero,cidade,lat,lon,meta,temp "
                      "FROM perfil WHERE id=1").fetchone()
    con.close()
    if row:
        estado["perfil"] = {"peso": row[0], "atividade": row[1], "genero": row[2],
                            "cidade": row[3], "lat": row[4], "lon": row[5]}
        estado["meta"] = row[6] or META_PADRAO
        estado["temp"] = row[7]

# ============================================================================
#  COMANDOS PARA O ESP32 (via MQTT)
# ============================================================================
def enviar_led(cor):
    mqtt_client.publish(TOPIC_CMD, json.dumps({"led": cor}))

def enviar_tara():
    mqtt_client.publish(TOPIC_CMD, json.dumps({"tara": 1}))

# ============================================================================
#  LEITURA DO ORION (padrão FIWARE - demonstra a integração)
# ============================================================================
def ler_peso_orion():
    """Retorna o valor atual do atributo 'peso' no Orion, ou None se falhar."""
    url = f"{ORION}/v2/entities/{ENTITY_ID}/attrs/peso/value"
    headers = {"fiware-service": FIWARE_SVC, "fiware-servicepath": FIWARE_PATH}
    try:
        r = requests.get(url, headers=headers, timeout=3)
        if r.status_code == 200:
            return float(r.json())
    except Exception as e:
        print("Erro ao ler Orion:", e)
    return None

# ============================================================================
#  MÁQUINA DE ESTADO DO CONSUMO  (núcleo do projeto)
# ============================================================================
def processar_peso(peso):
    agora = time.time()
    rollover_dia()

    # 0) Hibernado: não processa nada (a dock continua ligada, só não calcula)
    if estado["hibernado"]:
        return

    # 1) Re-baseline após tara/reativação.
    #    Em vez de ignorar só "a próxima leitura" (frágil: a leitura pode chegar
    #    ANTES de o ESP32 executar a tara, gastando a flag cedo demais e contando
    #    o salto pra zero como consumo), re-baseline TODAS as leituras durante uma
    #    janela de tempo após o comando de tara. Assim, quando o zero real chega,
    #    ele é absorvido como novo baseline, não como gole.
    if agora < estado["rebaseline_ate"]:
        estado["peso_ref"] = peso
        estado["garrafa_fora"] = (peso < LIMIAR_FORA)
        return

    # 2) Primeira leitura da vida: só estabelece a referência
    if estado["peso_ref"] is None:
        estado["peso_ref"] = peso
        return

    # 3) Garrafa REMOVIDA da dock.
    #    Como a tara é feita com a garrafa VAZIA em cima, remover a garrafa
    #    deixa o peso NEGATIVO. Esse é o sinal inequívoco de "garrafa fora".
    if peso < LIMIAR_FORA:
        estado["garrafa_fora"] = True       # mantém peso_ref de antes da remoção
        return                              # não conta nada enquanto fora

    # 4) Garrafa RETORNOU à dock após ter sido removida
    if estado["garrafa_fora"]:
        estado["garrafa_fora"] = False
        delta = estado["peso_ref"] - peso   # bebeu enquanto estava fora?
        if delta >= MIN_DELTA:
            registrar_consumo(delta, agora)
        estado["peso_ref"] = peso
        return

    # 5) Transição normal com a garrafa presente na dock
    delta = estado["peso_ref"] - peso
    if delta >= MIN_DELTA:                   # peso caiu -> GOLE
        registrar_consumo(delta, agora)
        estado["peso_ref"] = peso
    elif delta <= -MIN_DELTA:                # peso subiu -> REABASTECIMENTO
        db_evento("refill", -delta)
        estado["peso_ref"] = peso
    # else: ruído, ignora

def registrar_consumo(delta, agora):
    estado["consumo_dia"] += delta
    estado["ultima_atividade"] = agora
    db_evento("consumo", delta)
    print(f"GOLE: {delta:.0f} ml  (total dia: {estado['consumo_dia']:.0f} ml)")

def recalcular():
    """Atualiza pct, fatias e o estado do LED a partir do consumo."""
    meta = estado["meta"] or META_PADRAO
    estado["pct"] = round(estado["consumo_dia"] / meta * 100, 1)
    estado["fatias"] = min(4, int(estado["pct"] // 25))

    inativo = (time.time() - estado["ultima_atividade"]) > INATIVIDADE_S
    if estado["pct"] >= 100:
        novo_led = "verde_pulsando"
    elif inativo and estado["consumo_dia"] > 0:
        novo_led = "vermelho"
    else:
        novo_led = "verde"

    # Reenvia o comando de LED quando a cor muda OU a cada REENVIO_LED_S.
    # O reenvio periódico garante que o ESP32 saia do azul de boot e reflita
    # a cor certa mesmo se ligar depois do backend ou perder um comando MQTT.
    agora = time.time()
    mudou = (novo_led != estado["estado_led"])
    venceu = (agora - estado["ultimo_envio_led"]) > REENVIO_LED_S
    if mudou or venceu:
        estado["estado_led"] = novo_led
        estado["ultimo_envio_led"] = agora
        enviar_led(novo_led)

def rollover_dia():
    """Vira o dia à meia-noite: fecha o anterior e zera o consumo."""
    hoje = date.today().isoformat()
    if hoje != estado["data_dia"]:
        db_fechar_dia(estado["data_dia"], estado["consumo_dia"], estado["meta"])
        estado["streak"] = calcular_streak()
        estado["data_dia"] = hoje
        estado["consumo_dia"] = 0.0
        estado["peso_ref"] = None
        # Recalcula a meta do novo dia com a temperatura atual (trava p/ o dia,
        # evitando que oscilações de temperatura mudem pct/fatias no meio do dia)
        p = estado["perfil"]
        if p:
            temp = temperatura_atual(p["lat"], p["lon"]) if p.get("lat") else None
            estado["temp"] = temp
            estado["meta"] = calcular_meta(p["peso"], p["genero"], temp,
                                           p.get("atividade", "moderado"))["meta"]

# ============================================================================
#  LOOP DE FUNDO
# ============================================================================
def loop_processamento():
    ultimo_snapshot = 0
    while True:
        peso = ler_peso_orion()
        with lock:
            if peso is not None:
                estado["peso"] = peso
                estado["online"] = True
                estado["ultimo_pacote"] = time.time()
                processar_peso(peso)
            else:
                # sem leitura há > 30s -> marca offline
                if time.time() - estado["ultimo_pacote"] > 30:
                    estado["online"] = False
            recalcular()

            if time.time() - ultimo_snapshot >= SNAPSHOT_S:
                db_snapshot()
                ultimo_snapshot = time.time()
        time.sleep(POLL_S)

# ============================================================================
#  API REST (Flask)
# ============================================================================
app = Flask(__name__)
CORS(app)   # libera o React (evita erro de CORS no fetch)

@app.route("/")
def health():
    return jsonify({"servico": "waycare-backend", "ok": True})

@app.route("/status")
def status():
    """Estado atual da dock - o React faz polling deste endpoint."""
    with lock:
        return jsonify({
            "peso": round(estado["peso"], 1),
            "consumo_dia": round(estado["consumo_dia"], 1),
            "meta": estado["meta"],
            "pct": estado["pct"],
            "fatias": estado["fatias"],
            "estado_led": estado["estado_led"],
            "streak": estado["streak"],
            "garrafa_presente": not estado["garrafa_fora"],
            "online": estado["online"],
            "temp": estado["temp"],
            "cidade": estado["perfil"]["cidade"] if estado["perfil"] else None,
            "ts": datetime.now().isoformat(timespec="seconds"),
        })

@app.route("/history")
def history():
    """Série temporal p/ o gráfico do site (últimas N horas de snapshots)."""
    horas = int(request.args.get("horas", 24))
    desde = time.time() - horas * 3600
    con = sqlite3.connect(DB_PATH)
    rows = con.execute(
        "SELECT ts, consumo, pct FROM snapshots WHERE ts >= ? ORDER BY ts",
        (desde,)).fetchall()
    con.close()
    return jsonify([
        {"ts": datetime.fromtimestamp(t).isoformat(timespec="seconds"),
         "consumo": round(c, 1), "pct": round(p, 1)}
        for t, c, p in rows
    ])

@app.route("/perfil", methods=["GET"])
def get_perfil():
    """Devolve o perfil ativo e a meta calculada."""
    with lock:
        return jsonify({
            "perfil": estado["perfil"],
            "meta": estado["meta"],
            "temp": estado["temp"],
        })

@app.route("/perfil", methods=["POST"])
def post_perfil():
    """Define o perfil do dono da dock e recalcula a meta personalizada.
    Body JSON: { peso, altura, genero, cidade }  (lat/lon opcionais).
    A meta vira: ml/kg (por gênero) + ajuste por temperatura local."""
    dados = request.get_json(force=True)
    if "peso" not in dados:
        return jsonify({"ok": False, "erro": "peso é obrigatório"}), 400
    resultado = aplicar_perfil(dados)
    return jsonify({"ok": True, **resultado})

@app.route("/calcular-meta", methods=["POST"])
def calcular_meta_preview():
    """Só calcula e devolve a meta SEM salvar (preview no formulário)."""
    d = request.get_json(force=True)
    nome_in = d.get("cidade", "")
    lat, lon = d.get("lat"), d.get("lon")
    nome, aviso = nome_in, None
    if (lat is None or lon is None) and nome_in:
        lat, lon, nome_geo = geocodificar(nome_in)
        if lat is None:
            aviso = "Cidade não encontrada — digite uma cidade (não um país/estado)."
        else:
            nome = nome_geo
    temp = temperatura_atual(lat, lon) if lat is not None else None
    det = calcular_meta(float(d["peso"]), d.get("genero", "m"), temp,
                        d.get("atividade", "moderado"))
    return jsonify({**det, "temp": temp, "cidade": nome, "aviso": aviso})

@app.route("/tara", methods=["POST"])
def tara():
    """Botão de tara DO SITE. Manda o comando ao ESP32 e re-baseline o backend
    por uma janela de tempo (cobre a latência até o ESP32 executar a tara)."""
    with lock:
        estado["rebaseline_ate"] = time.time() + REBASELINE_S
    enviar_tara()
    db_evento("tara", 0)
    return jsonify({"ok": True, "msg": "Tara enviada ao dispositivo"})

# ============================================================================
#  MAIN
# ============================================================================
if __name__ == "__main__":
    init_db()
    carregar_perfil()                # restaura perfil/meta salvos
    estado["streak"] = calcular_streak()
    mqtt_client.connect(MQTT_HOST, MQTT_PORT, 60)
    mqtt_client.loop_start()
    threading.Thread(target=loop_processamento, daemon=True).start()
    # host 0.0.0.0 -> acessível pela rede; porta 5000
    app.run(host="0.0.0.0", port=5000)