# API WayCare Dock — Guia para o Front-end (React)

Referência da API REST servida pelo backend Python. O React **lê** o estado da hidratação e **envia** o comando de tara. Nada de MongoDB — o histórico vem de SQLite via estes endpoints.

Base URL (teste local): `http://IP_DA_VM:5000`

> Se o site estiver publicado em **HTTPS** (Vercel), o navegador bloqueia chamada a `http://` (mixed content). Nesse caso, exponha o backend via domínio com HTTPS (mesmo esquema do DarkSky: nginx + DuckDNS). Em teste local (site em `http://localhost`) o `http://IP:5000` funciona direto.

---

## `GET /status`
Estado atual da dock. É o endpoint que o React consulta em loop (polling a cada ~3s).

```json
{
  "peso": 1180.0,            // peso estável atual na dock (g)
  "consumo_dia": 820.0,      // ml consumidos hoje
  "meta": 2500.0,            // meta diária (ml)
  "pct": 32.8,               // % da meta
  "fatias": 1,               // fatias da meta atingidas (0–4)
  "estado_led": "verde",     // verde | verde_pulsando | vermelho
  "streak": 3,               // dias consecutivos batendo a meta
  "garrafa_presente": true,  // false = garrafa fora da dock
  "online": true,            // false = ESP32 sem enviar dados
  "ts": "2026-06-11T14:22:00"
}
```

## `GET /history?horas=24`
Série temporal para o gráfico. `horas` é opcional (padrão 24).

```json
[
  { "ts": "2026-06-11T08:00:00", "consumo": 0.0,   "pct": 0.0 },
  { "ts": "2026-06-11T09:00:00", "consumo": 350.0, "pct": 14.0 }
]
```

## `POST /tara`
Aciona a tara (zera a balança). **Aqui é a mudança da sprint:** não há mais botão físico — o site dispara a tara, o backend manda o comando ao ESP32 via MQTT e re-baseline o cálculo (o salto de peso pós-tara NÃO vira consumo).

```json
{ "ok": true, "msg": "Tara enviada ao dispositivo" }
```

---

## Meta personalizada (peso / gênero / temperatura)

A meta **não é mais fixa**. Cada dock tem um perfil (o dono do dispositivo) e a meta é calculada a partir dele + temperatura local (Open-Meteo, sem chave). O `/status` já devolve a `meta` e a `temp` atuais.

> Há **um dock físico por instância**. O perfil é único (o dono). Não é multiusuário com login dividindo o mesmo dock — a célula de carga não sabe quem está bebendo.

### `POST /perfil`
Salva o perfil e recalcula a meta. A meta passa a valer para o dock (pct, fatias, LED).

Body:
```json
{ "peso": 78, "genero": "m", "atividade": "moderado", "cidade": "São Paulo" }
```
`atividade`: `"sedentario"` | `"leve"` | `"moderado"` | `"intenso"`.

Resposta (com detalhamento de quanto cada fator somou):
```json
{ "meta": 3930, "base": 2730, "ajuste_temp": 500, "ajuste_atividade": 700,
  "temp": 33.0, "cidade": "São Paulo", "aviso": null }
```
Se a cidade não for encontrada (ex: usuário digitou um país), vem `"aviso"` preenchido e `temp: null`.

### `GET /perfil`
Devolve o perfil ativo e a meta.

### `POST /calcular-meta`
Igual ao `POST /perfil`, mas **só calcula e devolve, sem salvar** — use pra mostrar preview enquanto o usuário preenche. Mesmo body, mesma resposta com detalhamento.

> **Base científica (defensável, não prescrição médica):**
> - **Base por peso/sexo:** ml/kg clínico (35 masc. / 31 fem.), na ordem de grandeza da EFSA (2,5 L homem / 2,0 L mulher), que já pressupõe clima e atividade moderados.
> - **Temperatura:** ajuste só acima de 25°C (topo da faixa moderada da EFSA), +2,5% da base por °C, com **teto de +500 ml**. Não há constante científica de "ml por grau" — é estimativa transparente e limitada.
> - **Atividade:** ancorada na ACSM (0,4–0,8 L por hora de exercício): leve +350, moderado +700, intenso +1200 ml.
> - **Altura foi removida** — nenhuma referência de hidratação a usa de forma relevante.
> A meta é travada por dia (não muda com oscilação de temperatura no meio do dia). Mostre sempre como "meta sugerida".

---

## Origem de cada campo (honesto, p/ a banca)

| Campo | Origem |
|---|---|
| `peso` | **Real** — célula de carga via HX711, estabilizado no ESP32 |
| `consumo_dia` | **Calculado** — máquina de estado no backend (delta entre pesos estáveis) |
| `pct`, `fatias` | **Derivado** — consumo ÷ meta |
| `estado_led` | **Derivado** — regra de negócio no backend (também comanda o LED físico) |
| `streak` | **Calculado** — dias consecutivos com meta batida (SQLite) |

## Casos de borda já tratados no backend
- Garrafa levantada e recolocada → conta o gole.
- Garrafa retirada da dock (peso ~0) → **não** conta como consumo gigante.
- Reabastecimento (peso sobe) → registrado como `refill`, não como gole negativo.
- Tara → próximo peso vira baseline, não consumo.
- ESP32 offline → `online: false`.
