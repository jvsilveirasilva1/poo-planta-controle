"""Supervisório Streamlit do Fermentador Cervejeiro — consome telemetria.json e persiste em SQLite."""

from __future__ import annotations

import json
import os
from pathlib import Path
import sys
import tempfile

import streamlit as st

RAIZ_PROJETO = Path(__file__).resolve().parents[1]
if str(RAIZ_PROJETO) not in sys.path:
    sys.path.insert(0, str(RAIZ_PROJETO))

from supervisor.repositorio import RepositorioTelemetria, ValidadorTelemetria


LIMITE_OBSOLESCENCIA_SEG = 10.0


def gravar_comando_operador(dados_dir: Path, modo: str) -> None:
    """
    Grava atomicamente a solicitação de simulação/operador em dados/operador_comando.json.
    O Supervisório nunca escreve na entrada da planta (comando_planta.json);
    toda solicitação passa pela validação e decisão do Controlador C++.
    """
    modos_validos = {"normal", "falha_bomba", "sobrecarga_termica", "pausar_comandos"}
    if modo not in modos_validos:
        raise ValueError(f"Modo de operador invalido: {modo}")

    dados_dir = Path(dados_dir)
    dados_dir.mkdir(parents=True, exist_ok=True)
    destino = dados_dir / "operador_comando.json"
    conteudo = json.dumps({"versao": 1, "modo": modo}, ensure_ascii=False) + "\n"

    temporario = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w", encoding="utf-8", dir=dados_dir,
            prefix=f".{destino.name}.", suffix=".tmp", delete=False,
        ) as arq:
            temporario = Path(arq.name)
            arq.write(conteudo)
        os.replace(temporario, destino)
    finally:
        if temporario is not None:
            temporario.unlink(missing_ok=True)


def ler_modo_operador_atual(dados_dir: Path) -> str:
    caminho = Path(dados_dir) / "operador_comando.json"
    if not caminho.exists():
        return "normal"
    try:
        doc = json.loads(caminho.read_text(encoding="utf-8"))
        if isinstance(doc, dict) and doc.get("versao") == 1 and isinstance(doc.get("modo"), str):
            return doc["modo"]
    except Exception:
        pass
    return "normal"


def renderizar_historico(repositorio: RepositorioTelemetria, sessao_filtro: str | None = None) -> None:
    historico = repositorio.listar_historico(sessao=sessao_filtro, limite=200)
    st.subheader("Histórico Persistido (SQLite — Deduplicado por Sessão/Ciclo)")
    if not historico:
        st.info("Nenhum ciclo registrado no histórico SQLite ainda.")
        return

    st.dataframe(historico)

    if len(historico) >= 1:
        col_g1, col_g2 = st.columns(2)
        with col_g1:
            st.markdown("**Evolução do Nível de Mosto (`LT-01` — `%`)**")
            dados_nivel = {item["ciclo"]: item["nivel"] for item in historico}
            st.line_chart(dados_nivel)
        with col_g2:
            st.markdown("**Evolução da Temperatura de Fermentação (`TT-01` — `°C`)**")
            dados_temp = {item["ciclo"]: item["temperatura"] for item in historico}
            st.line_chart(dados_temp)

    csv_conteudo = repositorio.exportar_csv(sessao=sessao_filtro)
    st.download_button(
        label="Exportar Histórico Consultado (CSV)",
        data=csv_conteudo,
        file_name="historico_fermentador.csv",
        mime="text/csv",
    )


def _desenhar_painel(
    dados_dir: Path,
    caminho_telemetria: Path,
    caminho_banco: Path,
    simular_erro_tela: bool = False,
) -> None:
    repositorio = RepositorioTelemetria(caminho_banco)

    if simular_erro_tela:
        st.error(
            "Falha simulada na interface: documento de telemetria corrompido (JSON inválido). "
            "O histórico persistido no SQLite foi preservado."
        )
        renderizar_historico(repositorio)
        return

    try:
        telemetria = ValidadorTelemetria.carregar_arquivo(caminho_telemetria)
    except FileNotFoundError:
        st.warning(
            f"Aquisição indisponível: arquivo '{caminho_telemetria}' ainda não foi gerado pelo controlador C++."
        )
        renderizar_historico(repositorio)
        return
    except ValueError as erro:
        st.error(f"Falha de validação na telemetria recebida: {erro}")
        renderizar_historico(repositorio)
        return

    inserido_novo = repositorio.registrar_telemetria(telemetria)
    idade_seg = ValidadorTelemetria.calcular_idade_segundos(telemetria["timestamp"])
    obsoleta = ValidadorTelemetria.esta_obsoleta(
        telemetria["timestamp"], limite_segundos=LIMITE_OBSOLESCENCIA_SEG
    )

    if obsoleta:
        st.warning(
            f"Atenção — Dados Obsoletos: o snapshot atual tem {idade_seg:.1f}s "
            f"(limite configurado: {LIMITE_OBSOLESCENCIA_SEG:.0f}s). "
            "Verifique se a planta ou o controlador pararam."
        )

    col1, col2, col3, col4 = st.columns(4)
    col1.metric("Sessão", telemetria["sessao"][:12])
    col2.metric("Ciclo Atual", str(telemetria["ciclo"]))
    col3.metric("Último Comando Confirmado (ID)", str(telemetria["ultimo_comando"]))
    col4.metric("Idade da Aquisição", f"{idade_seg:.1f} s")

    st.subheader("Leituras Atuais da Dorna de Fermentação")
    st.dataframe(telemetria["leituras"])

    st.subheader("Atuação Observada na Planta")
    atuadores = telemetria["atuadores"]
    col_a1, col_a2, col_a3 = st.columns(3)
    col_a1.metric("Bomba de Mosto (%)", f"{atuadores['bomba']:.2f} %")
    col_a2.metric("Camisa Térmica / Aquecedor (%)", f"{atuadores['aquecedor']:.2f} %")
    status_cmd = (
        f"Modo seguro ({telemetria['motivo_seguro']})"
        if telemetria.get("motivo_seguro")
        else f"Confirmado ID #{telemetria['ultimo_comando']}"
    )
    col_a3.metric("Diagnóstico de Atuação", status_cmd)

    if telemetria.get("erro_comando"):
        st.error(f"Erro reportado pela planta no último comando: {telemetria['erro_comando']}")

    st.subheader("Alarmes e Diagnósticos de Domínio")
    alarmes = telemetria.get("alarmes", [])
    if not alarmes:
        st.success("Fermentador operando sem alarmes ativos.")
    else:
        for alarme in alarmes:
            texto_alarme = f"[{alarme['severidade'].upper()}] {alarme['codigo']}: {alarme['mensagem']}"
            if alarme["severidade"] == "critico":
                st.error(texto_alarme)
            else:
                st.warning(texto_alarme)

    st.caption(
        "Status da persistência neste ciclo: "
        + ("Novo registro gravado no SQLite." if inserido_novo else "Ciclo já existente (deduplicado).")
    )

    renderizar_historico(repositorio, sessao_filtro=telemetria["sessao"])


@st.fragment(run_every=1.0)
def _painel_auto_atualizado(
    dados_dir: Path,
    caminho_telemetria: Path,
    caminho_banco: Path,
    simular_erro_tela: bool = False,
) -> None:
    _desenhar_painel(dados_dir, caminho_telemetria, caminho_banco, simular_erro_tela)


def _renderizar_barra_simulacao(dados_dir: Path) -> bool:
    with st.sidebar:
        st.header("Simulação de Falhas e Alarmes")
        st.caption(
            "Os comandos são enviados ao Controlador C++ via `operador_comando.json` "
            "(preservando o único escritor de cada arquivo)."
        )
        modo_atual = ler_modo_operador_atual(dados_dir)
        st.info(f"Modo atual do controlador: **{modo_atual}**")

        if st.button("Restaurar Controle Automático", width="stretch", type="primary"):
            gravar_comando_operador(dados_dir, "normal")
            st.rerun()

        if st.button("Simular Falha na Bomba (NIVEL_BAIXO)", width="stretch"):
            gravar_comando_operador(dados_dir, "falha_bomba")
            st.rerun()

        if st.button("Simular Sobrecarga (TEMP_ALTA / NIVEL_ALTO)", width="stretch"):
            gravar_comando_operador(dados_dir, "sobrecarga_termica")
            st.rerun()

        if st.button("Simular Queda de Comando (Watchdog 5 ciclos)", width="stretch"):
            gravar_comando_operador(dados_dir, "pausar_comandos")
            st.rerun()

        st.divider()
        simular_erro_tela = st.toggle("Simular JSON inválido na tela", value=False)
    return simular_erro_tela


def main() -> None:
    dados_dir = Path(os.environ.get("POO_DADOS", "dados"))
    caminho_telemetria = dados_dir / "telemetria.json"
    caminho_banco = dados_dir / "historico.sqlite"

    simular_erro_tela = _renderizar_barra_simulacao(dados_dir)

    st.title("Supervisório — Fermentador Cervejeiro Automatizado")
    col_cab1, col_cab2 = st.columns([3, 1])
    with col_cab1:
        st.caption(f"Diretório de integração: {dados_dir} | Fonte: {caminho_telemetria.name}")
    with col_cab2:
        auto_atualizar = st.toggle("Atualização automática (1s)", value=True)

    if auto_atualizar:
        _painel_auto_atualizado(dados_dir, caminho_telemetria, caminho_banco, simular_erro_tela)
    else:
        _desenhar_painel(dados_dir, caminho_telemetria, caminho_banco, simular_erro_tela)


if __name__ == "__main__":
    main()
