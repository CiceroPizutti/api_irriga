# backend/app.py
import os
from fastapi import FastAPI, HTTPException, Header, Depends
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel, field_validator
from pymongo import MongoClient
from datetime import datetime, timedelta
import pytz
from dotenv import load_dotenv

# Carregar variáveis de ambiente
load_dotenv()

# === CONFIGURAÇÕES GLOBAIS ===
# Usamos variáveis de ambiente, mas fornecemos valores padrão para fácil teste.
MONGO_URI = os.getenv("MONGO_URI", "mongodb://localhost:27017")
DB_NAME = os.getenv("DB_NAME", "irrigacao_db")
TIMEZONE_STR = os.getenv("TIMEZONE", "America/Sao_Paulo")
# Chave API para autenticar o ESP32 (MUDAR NO .env!)
API_KEY = os.getenv("API_KEY", "minha-chave-secreta-esp32")

# === CONEXÃO COM MONGODB ===
try:
    client = MongoClient(MONGO_URI, serverSelectionTimeoutMS=5000)
    # Testar conexão
    client.admin.command('ping')
    print("Conexão com MongoDB estabelecida com sucesso.")
except Exception as e:
    print(f"Erro ao conectar ao MongoDB: {e}")
    # O app.py ainda pode iniciar, mas as operações do DB falharão se não estiver ativo.

db = client[DB_NAME]
hist_col = db["historico_umidade"] # Nova coleção focada apenas na umidade

# === FASTAPI APP ===
app = FastAPI(
    title="API de Umidade do Solo (ESP32)",
    description="Backend simplificado para registro e consulta de dados de umidade do sensor do ESP32.",
    version="1.0.0"
)

# === CORS (para frontend web ou testes) ===
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# === AUTENTICAÇÃO SIMPLES VIA HEADER ===
def check_api_key(x_api_key: str = Header(...)):
    """Verifica se a chave API no header é válida."""
    if x_api_key != API_KEY:
        # 401 Unauthorized
        raise HTTPException(status_code=401, detail="Chave API inválida ou ausente no header X-API-Key.")
    return x_api_key

# === MODELOS PYDANTIC ===

class UmidadeRegistro(BaseModel):
    """Modelo para o dado de umidade enviado pelo ESP32."""
    umidade: float

    @field_validator('umidade')
    def check_range(cls, v):
        """Garante que a umidade está entre 0 e 100."""
        if not (0 <= v <= 100):
            raise ValueError('O valor da umidade deve estar entre 0 e 100.')
        return v

class DadosGrafico(BaseModel):
    """Modelo de resposta para o endpoint de gráfico."""
    timestamps: list[str]
    umidades: list[float]
    media_ultima_hora: float
    amostras: int

# === ENDPOINTS ===

@app.get("/health")
def health():
    """Endpoint de verificação de saúde."""
    tz = pytz.timezone(TIMEZONE_STR)
    return {"status": "ok", "timestamp": datetime.now(tz).isoformat()}

# --- ENDPOINTS PARA O ESP32 (REGISTRO) ---

@app.post("/api/umidade/registrar")
def postar_umidade(item: UmidadeRegistro, api_key: str = Depends(check_api_key)):
    """Recebe o dado de umidade do ESP32 e o armazena no MongoDB."""
    
    tz = pytz.timezone(TIMEZONE_STR)
    # Formata o timestamp localmente para facilitar a leitura no DB
    now_local = datetime.now(tz).strftime("%Y-%m-%dT%H:%M:%S")

    doc = {
        "timestamp_local": now_local,
        "umidade": float(item.umidade),
        # Adicione o status da bomba se quiser registrar isso no futuro
    }

    try:
        hist_col.insert_one(doc)
        return {"status": "OK", "id": str(doc.get("_id", "")), "umidade": item.umidade}
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"Erro ao inserir no MongoDB: {e}")

# --- ENDPOINTS PARA O FRONTEND (VISUALIZAÇÃO) ---

@app.get("/historico/grafico", response_model=DadosGrafico)
def get_dados_grafico(
    horas: int = 24, # Limita a consulta às últimas X horas
    limit: int = 1000 # Limite máximo de pontos de dados
):
    """
    Retorna dados de umidade formatados para plotagem em gráfico.
    Usa um campo de tempo otimizado para o MongoDB.
    """
    try:
        tz = pytz.timezone(TIMEZONE_STR)
    except pytz.exceptions.UnknownTimeZoneError:
        tz = pytz.utc
    
    # Define o ponto de corte usando datetime.utcnow() para garantir que a data seja comparável,
    # embora o ESP32 envie uma string local formatada. Vamos usar string comparison mesmo:
    cutoff_dt = datetime.now(tz) - timedelta(hours=horas)
    cutoff_str = cutoff_dt.strftime("%Y-%m-%dT%H:%M:%S")

    query = {"timestamp_local": {"$gte": cutoff_str}}
    
    # Ordena por timestamp crescente para que o gráfico seja desenhado na ordem correta
    cursor = hist_col.find(query).sort("timestamp_local", 1).limit(limit)
    
    timestamps = []
    umidades = []
    soma_umidade = 0.0
    count = 0

    for doc in cursor:
        timestamps.append(doc["timestamp_local"])
        umidade_val = float(doc["umidade"])
        umidades.append(umidade_val)
        soma_umidade += umidade_val
        count += 1
        
    media = round(soma_umidade / count, 2) if count > 0 else 0.0

    return {
        "timestamps": timestamps,
        "umidades": umidades,
        "media_ultima_hora": media,
        "amostras": count
    }

@app.get("/historico")
def get_historico(
    limit: int = 50,
    api_key: str = Depends(check_api_key) # Protegido, pois é uma consulta mais detalhada
):
    """Retorna os últimos N registros brutos. Útil para debug."""
    cursor = hist_col.find({}).sort("timestamp_local", -1).limit(limit)
    items = []
    for doc in cursor:
        doc["_id"] = str(doc["_id"])
        items.append(doc)
    return items