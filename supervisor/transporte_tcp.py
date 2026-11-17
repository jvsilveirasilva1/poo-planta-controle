"""Extensão Bônus TCP: enquadramento NDJSON, tratamento de fragmentação/desconexão e Servidor TCP da Planta."""

from __future__ import annotations

import json
from pathlib import Path
import socket
import threading
from typing import Any

from planta.simulador import Planta, gravar_json


class EnquadradorTcp:
    """
    Gerencia o enquadramento (framing) de mensagens JSON sobre stream TCP (NDJSON terminado por '\\n').
    Reconstitui mensagens divididas em múltiplos pacotes (fragmentação) e separa mensagens
    concatenadas no mesmo recv() (coalescência).
    """

    def __init__(self, tamanho_maximo: int = 65536) -> None:
        if tamanho_maximo < 64:
            raise ValueError("tamanho_maximo deve ser >= 64 bytes")
        self.tamanho_maximo = tamanho_maximo
        self._buffer = bytearray()

    def receber_bytes(self, dados: bytes) -> list[str]:
        if not dados:
            return []
        self._buffer.extend(dados)
        mensagens: list[str] = []

        while True:
            indice = self._buffer.find(b"\n")
            if indice == -1:
                break
            bruto = bytes(self._buffer[:indice])
            del self._buffer[: indice + 1]
            if bruto.endswith(b"\r"):
                bruto = bruto[:-1]
            if bruto:
                mensagens.append(bruto.decode("utf-8"))

        if len(self._buffer) > self.tamanho_maximo:
            self._buffer.clear()
            raise BufferError("Quadro TCP excedeu o limite maximo sem delimitador de linha")

        return mensagens

    def limpar_ao_desconectar(self) -> None:
        self._buffer.clear()

    @property
    def bytes_pendentes(self) -> int:
        return len(self._buffer)

    @staticmethod
    def enquadrar(documento: dict[str, Any] | str) -> bytes:
        if isinstance(documento, dict):
            texto = json.dumps(documento, ensure_ascii=False, allow_nan=False)
        else:
            texto = documento.replace("\r", "").replace("\n", "")
        if not texto:
            raise ValueError("Mensagem TCP nao pode ser vazia")
        return (texto + "\n").encode("utf-8")


class ServidorTcpPlanta:
    """
    Servidor TCP que encapsula a Planta fornecida preservando 100% do contrato de
    planta.json e comando_planta.json, suportando múltiplos clientes, fragmentação de
    pacotes TCP e reconexão sem perder a sessão nem o ciclo atual.
    """

    def __init__(
        self,
        planta: Planta,
        diretorio_dados: Path,
        host: str = "127.0.0.1",
        porta: int = 0,
    ) -> None:
        self.planta = planta
        self.diretorio_dados = Path(diretorio_dados)
        self.diretorio_dados.mkdir(parents=True, exist_ok=True)
        self.host = host
        self._lock = threading.Lock()

        self._caminho_cmd = self.diretorio_dados / "comando_planta.json"
        self._caminho_planta = self.diretorio_dados / "planta.json"
        self._ultimo_estado = self.planta.passo(self._caminho_cmd)
        gravar_json(self._caminho_planta, self._ultimo_estado)

        self._sock_servidor = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._sock_servidor.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock_servidor.bind((host, porta))
        self._sock_servidor.listen(8)
        self._sock_servidor.settimeout(0.2)
        self.porta: int = self._sock_servidor.getsockname()[1]

        self._ativo = False
        self._thread_aceitacao: threading.Thread | None = None

    def iniciar_em_background(self) -> None:
        if self._ativo:
            return
        self._ativo = True
        self._thread_aceitacao = threading.Thread(target=self._loop_aceitacao, daemon=True)
        self._thread_aceitacao.start()

    def encerrar(self) -> None:
        self._ativo = False
        if self._thread_aceitacao is not None:
            self._thread_aceitacao.join(timeout=2.0)
        try:
            self._sock_servidor.close()
        except OSError:
            pass

    def avancar_ciclo(self) -> dict[str, Any]:
        with self._lock:
            self._ultimo_estado = self.planta.passo(self._caminho_cmd)
            gravar_json(self._caminho_planta, self._ultimo_estado)
            return dict(self._ultimo_estado)

    def estado_atual(self) -> dict[str, Any]:
        with self._lock:
            return dict(self._ultimo_estado)

    def _loop_aceitacao(self) -> None:
        while self._ativo:
            try:
                conexao, _ = self._sock_servidor.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            t = threading.Thread(target=self._atender_cliente, args=(conexao,), daemon=True)
            t.start()

    def _atender_cliente(self, conexao: socket.socket) -> None:
        enquadrador = EnquadradorTcp()
        conexao.settimeout(1.0)
        try:
            while self._ativo:
                try:
                    fragmento = conexao.recv(256)
                except socket.timeout:
                    continue
                if not fragmento:
                    break
                mensagens = enquadrador.receber_bytes(fragmento)
                for msg in mensagens:
                    resposta = self._processar_quadro(msg)
                    conexao.sendall(EnquadradorTcp.enquadrar(resposta))
        except (OSError, ValueError, BufferError):
            pass
        finally:
            enquadrador.limpar_ao_desconectar()
            try:
                conexao.close()
            except OSError:
                pass

    def _processar_quadro(self, msg_json: str) -> dict[str, Any]:
        doc = json.loads(msg_json)
        if not isinstance(doc, dict):
            return {"status": "erro", "motivo": "quadro deve ser objeto JSON"}

        if doc.get("acao") == "OBTER_PLANTA":
            with self._lock:
                return dict(self._ultimo_estado)

        if doc.get("acao") == "OBTER_TELEMETRIA":
            caminho_tel = self.diretorio_dados / "telemetria.json"
            if caminho_tel.exists():
                return json.loads(caminho_tel.read_text(encoding="utf-8"))
            return {"status": "erro", "motivo": "telemetria ausente"}

        # Caso contrário, trata como comando_planta.json enviado pelo Controlador
        with self._lock:
            gravar_json(self._caminho_cmd, doc)
            return {"status": "ok", "id_recebido": doc.get("id")}


class ClienteTcpSupervisor:
    """Cliente TCP resiliente que permite ao Supervisório consultar a telemetria via socket TCP."""

    def __init__(self, host: str, porta: int, timeout: float = 2.0) -> None:
        self.host = host
        self.porta = porta
        self.timeout = timeout

    def requisitar_json(self, acao: str = "OBTER_TELEMETRIA") -> dict[str, Any]:
        enquadrador = EnquadradorTcp()
        with socket.create_connection((self.host, self.porta), timeout=self.timeout) as sock:
            sock.sendall(EnquadradorTcp.enquadrar({"acao": acao}))
            while True:
                bloco = sock.recv(512)
                if not bloco:
                    raise ConnectionError("Conexao encerrada pelo servidor TCP antes do quadro completo")
                quadros = enquadrador.receber_bytes(bloco)
                if quadros:
                    return json.loads(quadros[0])


def main() -> int:  # pragma: no cover
    import argparse
    import time

    parser = argparse.ArgumentParser(description="Servidor TCP da Planta do Fermentador Cervejeiro")
    parser.add_argument("--dados", type=Path, default=Path("dados"))
    parser.add_argument("--host", type=str, default="127.0.0.1")
    parser.add_argument("--porta", type=int, default=9100)
    parser.add_argument("--intervalo", type=float, default=1.0)
    parser.add_argument("--seed", type=int, default=7)
    args = parser.parse_args()

    planta = Planta(seed=args.seed)
    servidor = ServidorTcpPlanta(planta, args.dados, host=args.host, porta=args.porta)
    servidor.iniciar_em_background()
    print(f"Servidor TCP da Planta ativo em {args.host}:{servidor.porta} (sessao={planta.sessao})", flush=True)
    try:
        while True:
            time.sleep(args.intervalo)
            estado = servidor.avancar_ciclo()
            print(
                f"[TCP] ciclo={estado['ciclo']} nivel={planta.nivel:.1f}% "
                f"temperatura={planta.temperatura:.1f}C ack={planta.ultimo_comando}",
                flush=True,
            )
    except KeyboardInterrupt:
        pass
    finally:
        servidor.encerrar()
    return 0


if __name__ == "__main__":  # pragma: no cover
    import sys

    sys.exit(main())
