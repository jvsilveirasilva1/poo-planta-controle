"""Testes autorais Python para validação de telemetria, Repository SQLite e integração E2E."""

from __future__ import annotations

from datetime import datetime, timezone
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from planta.simulador import Planta, gravar_json
from supervisor.repositorio import RepositorioTelemetria, ValidadorTelemetria
from supervisor.transporte_tcp import (
    ClienteTcpSupervisor,
    EnquadradorTcp,
    ServidorTcpPlanta,
)

RAIZ = Path(__file__).resolve().parents[1]
APP = RAIZ / "supervisor" / "app.py"


def criar_telemetria_exemplo(
    sessao: str = "sessao-ferm",
    ciclo: int = 1,
    nivel: float = 60.0,
    temperatura: float = 35.0,
    alarmes: list | None = None,
) -> dict:
    return {
        "versao": 1,
        "sessao": sessao,
        "ciclo": ciclo,
        "timestamp": "2026-10-21T18:00:00Z",
        "leituras": [
            {"tag": "LT-01", "tipo": "nivel", "valor": nivel, "unidade": "%", "status": "operando"},
            {"tag": "TT-01", "tipo": "temperatura", "valor": temperatura, "unidade": "C", "status": "operando"},
        ],
        "atuadores": {"bomba": 33.33, "aquecedor": 40.0},
        "ultimo_comando": 1,
        "erro_comando": None,
        "motivo_seguro": None,
        "alarmes": alarmes if alarmes is not None else [],
    }


class TestValidadorTelemetriaAutoral(unittest.TestCase):
    def test_validacao_caso_comum_e_fronteiras_de_valor(self):
        doc = criar_telemetria_exemplo(nivel=0.0, temperatura=100.0)
        validado = ValidadorTelemetria.validar_documento(doc)
        self.assertEqual(validado["ciclo"], 1)
        self.assertEqual(validado["leituras"][0]["valor"], 0.0)
        self.assertEqual(validado["leituras"][1]["valor"], 100.0)

    def test_fronteira_de_obsolescencia_timestamp(self):
        agora = datetime(2026, 10, 21, 18, 0, 10, tzinfo=timezone.utc)
        # Idade exata de 10.0s para limite de 10.0s -> NÃO obsoleta
        self.assertFalse(
            ValidadorTelemetria.esta_obsoleta("2026-10-21T18:00:00Z", limite_segundos=10.0, agora_utc=agora)
        )
        # Idade de 10.0s para limite de 9.9s -> Obsoleta
        self.assertTrue(
            ValidadorTelemetria.esta_obsoleta("2026-10-21T18:00:00Z", limite_segundos=9.9, agora_utc=agora)
        )
        with self.assertRaises(ValueError):
            ValidadorTelemetria.esta_obsoleta("2026-10-21T18:00:00Z", limite_segundos=-1.0, agora_utc=agora)

    def test_rejeicao_de_telemetrias_invalidas(self):
        # Versão booleana (True == 1 em Python se não verificar tipo estrito)
        doc_bool = criar_telemetria_exemplo()
        doc_bool["versao"] = True
        with self.assertRaises(ValueError):
            ValidadorTelemetria.validar_documento(doc_bool)

        # Leitura com status falha
        doc_falha = criar_telemetria_exemplo()
        doc_falha["leituras"][0]["status"] = "falha"
        with self.assertRaises(ValueError):
            ValidadorTelemetria.validar_documento(doc_falha)

        # Atuador fora de faixa
        doc_atuador = criar_telemetria_exemplo()
        doc_atuador["atuadores"]["bomba"] = 105.0
        with self.assertRaises(ValueError):
            ValidadorTelemetria.validar_documento(doc_atuador)


class TestRepositorioSQLiteAutoral(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.caminho_db = Path(self.tmp.name) / "historico.sqlite"
        self.repo = RepositorioTelemetria(self.caminho_db)

    def test_persistencia_consulta_deduplicacao_e_exportacao_csv(self):
        t1 = criar_telemetria_exemplo(sessao="s1", ciclo=1, nivel=52.5, temperatura=33.5)
        t2 = criar_telemetria_exemplo(
            sessao="s1",
            ciclo=2,
            nivel=20.0,
            temperatura=34.0,
            alarmes=[{"codigo": "NIVEL_BAIXO", "mensagem": "Nivel baixo", "severidade": "aviso"}],
        )

        # Primeira gravação do ciclo 1: deve inserir (True)
        self.assertTrue(self.repo.registrar_telemetria(t1))
        # Segunda gravação do MESMO (sessao="s1", ciclo=1): deve deduplicar e retornar False
        self.assertFalse(self.repo.registrar_telemetria(t1))
        # Gravação do ciclo 2: deve inserir (True)
        self.assertTrue(self.repo.registrar_telemetria(t2))

        historico = self.repo.listar_historico(sessao="s1")
        self.assertEqual(len(historico), 2, "Releituras da tela nao podem duplicar linhas no SQLite")
        self.assertEqual(historico[0]["ciclo"], 1)
        self.assertEqual(historico[0]["nivel"], 52.5)
        self.assertEqual(historico[1]["ciclo"], 2)
        self.assertEqual(historico[1]["codigos_alarmes"], "NIVEL_BAIXO")

        # Exportação CSV contém os dados consultados
        csv_texto = self.repo.exportar_csv(sessao="s1")
        linhas_csv = [linha for linha in csv_texto.strip().split("\n") if linha]
        self.assertEqual(len(linhas_csv), 3, "CSV deve ter cabecalho + 2 linhas deduplicadas")
        self.assertIn("52.5", linhas_csv[1])
        self.assertIn("NIVEL_BAIXO", linhas_csv[2])

    def test_falha_de_validacao_preserva_historico_anterior_no_sqlite(self):
        valido = criar_telemetria_exemplo(sessao="s1", ciclo=1, nivel=60.0)
        self.assertTrue(self.repo.registrar_telemetria(valido))

        invalido = criar_telemetria_exemplo(sessao="s1", ciclo=2, nivel=-10.0)
        with self.assertRaises(ValueError):
            self.repo.registrar_telemetria(invalido)

        historico = self.repo.listar_historico(sessao="s1")
        self.assertEqual(len(historico), 1, "Falha de validacao nao pode inserir registro parcial")
        self.assertEqual(historico[0]["nivel"], 60.0)


class TestCaminhoCompletoPlantaControladorTela(unittest.TestCase):
    def test_caminho_completo_planta_controlador_telemetria_sqlite_e_streamlit(self):
        from streamlit.testing.v1 import AppTest

        with tempfile.TemporaryDirectory() as diretorio:
            dados = Path(diretorio)
            planta = Planta(seed=7, sessao="integracao-e2e")

            # 1. Planta gera primeiro ciclo (nível inicial ~49%, temperatura ~24.8C)
            estado_1 = planta.passo(dados / "comando_planta.json")
            gravar_json(dados / "planta.json", estado_1)

            # 2. Controlador C++ processa ciclo 1, emite comando_planta.json e telemetria.json
            bin_controlador = os.environ.get("POO_CONTROLADOR", str((RAIZ / "build/controlador").resolve()))
            res_ctrl_1 = subprocess.run(
                [bin_controlador, "--dados", str(dados), "--uma-vez"],
                capture_output=True, text=True, timeout=10,
            )
            self.assertEqual(res_ctrl_1.returncode, 0, f"Controlador falhou: {res_ctrl_1.stderr}")
            self.assertTrue((dados / "comando_planta.json").exists())
            self.assertTrue((dados / "telemetria.json").exists())

            # 3. Planta executa segundo ciclo aplicando o comando do controlador e confirmando ID=1
            estado_2 = planta.passo(dados / "comando_planta.json")
            self.assertEqual(estado_2["ultimo_comando"], 1)
            gravar_json(dados / "planta.json", estado_2)

            # 4. Controlador C++ processa ciclo 2
            res_ctrl_2 = subprocess.run(
                [bin_controlador, "--dados", str(dados), "--uma-vez"],
                capture_output=True, text=True, timeout=10,
            )
            self.assertEqual(res_ctrl_2.returncode, 0)

            # 5. Supervisório Streamlit é carregado duas vezes seguidas (simulando refresh da tela)
            with patch.dict(os.environ, {"POO_DADOS": str(dados)}):
                tela_1 = AppTest.from_file(str(APP)).run(timeout=20)
                self.assertFalse(tela_1.exception)
                tela_2 = AppTest.from_file(str(APP)).run(timeout=20)
                self.assertFalse(tela_2.exception)

            # 6. Verifica que o SQLite gravou apenas 1 registro para o ciclo 2 (sem duplicar no refresh)
            repo = RepositorioTelemetria(dados / "historico.sqlite")
            historico = repo.listar_historico(sessao="integracao-e2e")
            self.assertEqual(len(historico), 1)
            self.assertEqual(historico[0]["ciclo"], 2)
            self.assertEqual(historico[0]["ultimo_comando"], 1)


class TestBonusTcpIntegracao(unittest.TestCase):
    def test_enquadrador_tcp_fragmentacao_coalescencia_e_overflow(self):
        enq = EnquadradorTcp(tamanho_maximo=128)
        quadro = EnquadradorTcp.enquadrar({"versao": 1, "ciclo": 10})
        # Envia de 4 em 4 bytes
        extraidos = []
        for i in range(0, len(quadro), 4):
            extraidos.extend(enq.receber_bytes(quadro[i : i + 4]))
        self.assertEqual(len(extraidos), 1)
        self.assertEqual(enq.bytes_pendentes, 0)

        # Coalescência de dois quadros no mesmo pacote
        duplo = EnquadradorTcp.enquadrar({"a": 1}) + EnquadradorTcp.enquadrar({"b": 2})
        self.assertEqual(len(enq.receber_bytes(duplo)), 2)

        # Fragmento incompleto limpo na desconexão
        enq.receber_bytes(b'{"parcial":')
        self.assertGreater(enq.bytes_pendentes, 0)
        enq.limpar_ao_desconectar()
        self.assertEqual(enq.bytes_pendentes, 0)

        # Estouro de buffer sem '\n'
        with self.assertRaises(BufferError):
            enq.receber_bytes(b"X" * 200)

    def test_integracao_tcp_planta_controlador_cpp_e_fragmentacao_em_socket(self):
        import socket

        with tempfile.TemporaryDirectory() as diretorio:
            dados = Path(diretorio)
            planta = Planta(seed=7, sessao="sessao-tcp-bonus")
            servidor = ServidorTcpPlanta(planta, dados, host="127.0.0.1", porta=0)
            servidor.iniciar_em_background()
            self.addCleanup(servidor.encerrar)

            bin_controlador = os.environ.get("POO_CONTROLADOR", str((RAIZ / "build/controlador").resolve()))
            endereco = f"127.0.0.1:{servidor.porta}"

            # 1. Controlador C++ conecta via TCP no ciclo 1 e envia comando id=1
            res1 = subprocess.run(
                [bin_controlador, "--dados", str(dados), "--tcp", endereco, "--uma-vez"],
                capture_output=True, text=True, timeout=10,
            )
            self.assertEqual(res1.returncode, 0, f"Controlador TCP falhou: {res1.stderr}")

            # 2. Planta avança para o ciclo 2 confirmando o comando id=1 recebido via TCP
            estado_c2 = servidor.avancar_ciclo()
            self.assertEqual(estado_c2["ultimo_comando"], 1)

            # 3. Envia um comando fragmentado em pequenos pedaços via socket TCP direto e reconecta
            cmd_frag = EnquadradorTcp.enquadrar(
                {"versao": 1, "sessao": "sessao-tcp-bonus", "id": 2, "atuadores": {"bomba": 50.0, "aquecedor": 50.0}}
            )
            with socket.create_connection(("127.0.0.1", servidor.porta), timeout=2.0) as sock:
                for i in range(0, len(cmd_frag), 7):
                    sock.sendall(cmd_frag[i : i + 7])
                resp = sock.recv(256)
                self.assertIn(b'"status": "ok"', resp)

            estado_c3 = servidor.avancar_ciclo()
            self.assertEqual(estado_c3["ultimo_comando"], 2)

            # 4. ClienteTcpSupervisor requisita a telemetria gerada pelo C++ via TCP
            cliente_sup = ClienteTcpSupervisor("127.0.0.1", servidor.porta)
            tel_tcp = cliente_sup.requisitar_json("OBTER_TELEMETRIA")
            self.assertEqual(tel_tcp["sessao"], "sessao-tcp-bonus")


if __name__ == "__main__":
    unittest.main()
