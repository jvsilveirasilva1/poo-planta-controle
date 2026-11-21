"""Validação de telemetria e padrão Repository com SQLite deduplicado por (sessao, ciclo)."""

from __future__ import annotations

import csv
from datetime import datetime, timezone
import io
import json
import math
from pathlib import Path
import sqlite3
from typing import Any


def _eh_inteiro_estrito(valor: Any) -> bool:
    return type(valor) is int


def _eh_numero_finito(valor: Any, minimo: float = 0.0, maximo: float = 100.0) -> bool:
    return (
        type(valor) in (int, float)
        and math.isfinite(float(valor))
        and minimo <= float(valor) <= maximo
    )


class ValidadorTelemetria:
    """Valida o envelope e o conteúdo de dados/telemetria.json no supervisório."""

    @staticmethod
    def validar_documento(doc: Any) -> dict[str, Any]:
        if not isinstance(doc, dict):
            raise ValueError("telemetria deve ser um objeto JSON")

        obrigatorios = [
            "versao", "sessao", "ciclo", "timestamp",
            "leituras", "atuadores", "ultimo_comando", "alarmes",
        ]
        for chave in obrigatorios:
            if chave not in doc:
                raise ValueError(f"campo obrigatorio ausente na telemetria: {chave}")

        if not _eh_inteiro_estrito(doc["versao"]) or doc["versao"] != 1:
            raise ValueError("versao da telemetria deve ser inteiro 1")

        if not isinstance(doc["sessao"], str) or not doc["sessao"].strip():
            raise ValueError("sessao da telemetria deve ser string nao vazia")

        if not _eh_inteiro_estrito(doc["ciclo"]) or doc["ciclo"] <= 0:
            raise ValueError("ciclo da telemetria deve ser inteiro positivo")

        ValidadorTelemetria.converter_timestamp_utc(doc["timestamp"])

        leituras = doc["leituras"]
        if not isinstance(leituras, list) or len(leituras) < 2:
            raise ValueError("leituras deve ser lista com pelo menos duas grandezas")

        mapa_grandezas: dict[str, dict[str, Any]] = {}
        for item in leituras:
            if not isinstance(item, dict):
                raise ValueError("cada leitura deve ser um objeto")
            tag = item.get("tag")
            tipo = item.get("tipo")
            valor = item.get("valor")
            unidade = item.get("unidade")
            status = item.get("status")
            if not isinstance(tag, str) or not tag:
                raise ValueError("tag da leitura invalida")
            if status != "operando":
                raise ValueError(f"leitura com status invalido: {status}")
            if not _eh_numero_finito(valor, 0.0, 100.0):
                raise ValueError("valor da leitura fora de [0, 100]")
            if tipo == "nivel" and unidade == "%":
                mapa_grandezas["nivel"] = item
            elif tipo == "temperatura" and unidade == "C":
                mapa_grandezas["temperatura"] = item
            else:
                raise ValueError(f"tipo/unidade incompativeis na leitura: {tipo} ({unidade})")

        if "nivel" not in mapa_grandezas or "temperatura" not in mapa_grandezas:
            raise ValueError("telemetria deve conter leituras de nivel e temperatura")

        atuadores = doc["atuadores"]
        if (
            not isinstance(atuadores, dict)
            or set(atuadores.keys()) != {"bomba", "aquecedor"}
            or not _eh_numero_finito(atuadores["bomba"], 0.0, 100.0)
            or not _eh_numero_finito(atuadores["aquecedor"], 0.0, 100.0)
        ):
            raise ValueError("atuadores da telemetria invalidos")

        if not _eh_inteiro_estrito(doc["ultimo_comando"]) or doc["ultimo_comando"] < 0:
            raise ValueError("ultimo_comando deve ser inteiro >= 0")

        if not isinstance(doc["alarmes"], list):
            raise ValueError("alarmes deve ser uma lista")
        for alarme in doc["alarmes"]:
            if (
                not isinstance(alarme, dict)
                or not isinstance(alarme.get("codigo"), str)
                or not isinstance(alarme.get("mensagem"), str)
                or not isinstance(alarme.get("severidade"), str)
            ):
                raise ValueError("item de alarme invalido na telemetria")

        return doc

    @staticmethod
    def carregar_arquivo(caminho: Path) -> dict[str, Any]:
        caminho = Path(caminho)
        if not caminho.exists():
            raise FileNotFoundError(f"Arquivo de telemetria ausente: {caminho}")
        try:
            conteudo = json.loads(caminho.read_text(encoding="utf-8"))
        except json.JSONDecodeError as erro:
            raise ValueError(f"JSON de telemetria invalido: {erro}") from erro
        return ValidadorTelemetria.validar_documento(conteudo)

    @staticmethod
    def converter_timestamp_utc(timestamp_str: Any) -> datetime:
        if not isinstance(timestamp_str, str) or len(timestamp_str) != 20 or not timestamp_str.endswith("Z"):
            raise ValueError("timestamp deve estar no formato UTC AAAA-MM-DDTHH:MM:SSZ")
        try:
            dt = datetime.strptime(timestamp_str, "%Y-%m-%dT%H:%M:%SZ")
            return dt.replace(tzinfo=timezone.utc)
        except ValueError as erro:
            raise ValueError(f"timestamp UTC invalido: {timestamp_str}") from erro

    @staticmethod
    def calcular_idade_segundos(timestamp_str: str, agora_utc: datetime | None = None) -> float:
        instante = ValidadorTelemetria.converter_timestamp_utc(timestamp_str)
        referencia = agora_utc or datetime.now(timezone.utc)
        return max(0.0, (referencia - instante).total_seconds())

    @staticmethod
    def esta_obsoleta(
        timestamp_str: str,
        limite_segundos: float = 10.0,
        agora_utc: datetime | None = None,
    ) -> bool:
        if limite_segundos < 0:
            raise ValueError("limite_segundos nao pode ser negativo")
        idade = ValidadorTelemetria.calcular_idade_segundos(timestamp_str, agora_utc)
        return idade > limite_segundos


class RepositorioTelemetria:
    """
    Padrão Repository: isola a persistência SQLite do histórico de telemetria,
    garantindo deduplicação estrita por (sessao, ciclo) e exportação CSV.
    """

    def __init__(self, caminho_banco: Path) -> None:
        self.caminho_banco = Path(caminho_banco)
        self.caminho_banco.parent.mkdir(parents=True, exist_ok=True)
        self._inicializar_esquema()

    def _conectar(self) -> sqlite3.Connection:
        conn = sqlite3.connect(self.caminho_banco)
        conn.row_factory = sqlite3.Row
        return conn

    def _inicializar_esquema(self) -> None:
        with self._conectar() as conn:
            conn.execute(
                """
                CREATE TABLE IF NOT EXISTS historico_telemetria (
                    sessao TEXT NOT NULL,
                    ciclo INTEGER NOT NULL,
                    timestamp TEXT NOT NULL,
                    tag_nivel TEXT NOT NULL,
                    nivel REAL NOT NULL,
                    unidade_nivel TEXT NOT NULL,
                    tag_temperatura TEXT NOT NULL,
                    temperatura REAL NOT NULL,
                    unidade_temperatura TEXT NOT NULL,
                    bomba REAL NOT NULL,
                    aquecedor REAL NOT NULL,
                    ultimo_comando INTEGER NOT NULL,
                    erro_comando TEXT,
                    motivo_seguro TEXT,
                    alarmes_json TEXT NOT NULL,
                    PRIMARY KEY (sessao, ciclo)
                )
                """
            )

    def registrar_telemetria(self, documento: dict[str, Any]) -> bool:
        """
        Valida e persiste um snapshot de telemetria se o par (sessao, ciclo) for inédito.
        Retorna True quando um novo registro é inserido e False quando é duplicado.
        """
        doc = ValidadorTelemetria.validar_documento(documento)
        leitura_nivel = next(l for l in doc["leituras"] if l["tipo"] == "nivel")
        leitura_temp = next(l for l in doc["leituras"] if l["tipo"] == "temperatura")

        with self._conectar() as conn:
            cursor = conn.execute(
                """
                INSERT OR IGNORE INTO historico_telemetria (
                    sessao, ciclo, timestamp,
                    tag_nivel, nivel, unidade_nivel,
                    tag_temperatura, temperatura, unidade_temperatura,
                    bomba, aquecedor, ultimo_comando,
                    erro_comando, motivo_seguro, alarmes_json
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    doc["sessao"],
                    doc["ciclo"],
                    doc["timestamp"],
                    leitura_nivel["tag"],
                    float(leitura_nivel["valor"]),
                    leitura_nivel["unidade"],
                    leitura_temp["tag"],
                    float(leitura_temp["valor"]),
                    leitura_temp["unidade"],
                    float(doc["atuadores"]["bomba"]),
                    float(doc["atuadores"]["aquecedor"]),
                    int(doc["ultimo_comando"]),
                    doc.get("erro_comando"),
                    doc.get("motivo_seguro"),
                    json.dumps(doc["alarmes"], ensure_ascii=False),
                ),
            )
            return cursor.rowcount == 1

    def listar_historico(self, sessao: str | None = None, limite: int = 200) -> list[dict[str, Any]]:
        if limite <= 0:
            raise ValueError("limite de consulta deve ser positivo")

        with self._conectar() as conn:
            if sessao:
                linhas = conn.execute(
                    """
                    SELECT * FROM historico_telemetria
                    WHERE sessao = ?
                    ORDER BY ciclo ASC
                    LIMIT ?
                    """,
                    (sessao, limite),
                ).fetchall()
            else:
                linhas = conn.execute(
                    """
                    SELECT * FROM historico_telemetria
                    ORDER BY rowid ASC
                    LIMIT ?
                    """,
                    (limite,),
                ).fetchall()

        resultado: list[dict[str, Any]] = []
        for linha in linhas:
            item = dict(linha)
            alarmes_lista = json.loads(item.pop("alarmes_json"))
            item["alarmes"] = alarmes_lista
            item["codigos_alarmes"] = ", ".join(a["codigo"] for a in alarmes_lista) if alarmes_lista else "NENHUM"
            resultado.append(item)
        return resultado

    def exportar_csv(self, sessao: str | None = None, limite: int = 1000) -> str:
        registros = self.listar_historico(sessao=sessao, limite=limite)
        colunas = [
            "sessao", "ciclo", "timestamp",
            "tag_nivel", "nivel", "unidade_nivel",
            "tag_temperatura", "temperatura", "unidade_temperatura",
            "bomba", "aquecedor", "ultimo_comando",
            "erro_comando", "motivo_seguro", "codigos_alarmes",
        ]
        buffer = io.StringIO()
        escritor = csv.DictWriter(buffer, fieldnames=colunas, extrasaction="ignore", lineterminator="\n")
        escritor.writeheader()
        for reg in registros:
            escritor.writerow(reg)
        return buffer.getvalue()
