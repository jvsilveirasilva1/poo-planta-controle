"""Supervisório Streamlit do Fermentador Cervejeiro — consome telemetria.json e persiste em SQLite."""

from __future__ import annotations

import os
from pathlib import Path
import sys

import streamlit as st

RAIZ_PROJETO = Path(__file__).resolve().parents[1]
if str(RAIZ_PROJETO) not in sys.path:
    sys.path.insert(0, str(RAIZ_PROJETO))

from supervisor.repositorio import RepositorioTelemetria, ValidadorTelemetria


LIMITE_OBSOLESCENCIA_SEG = 10.0


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
        label="Exportar Histórico Consultihado (CSV)",
        data=csv_conteudo,
        file_name="historico_fermentador.csv",
        mime="text/csv",
    )


def _desenhar_painel(dados_dir: Path, caminho_telemetria: Path, caminho_banco: Path) -> None:
    repositorio = RepositorioTelemetria(caminho_banco)

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
def _painel_auto_atualizado(dados_dir: Path, caminho_telemetria: Path, caminho_banco: Path) -> None:
    _desenhar_painel(dados_dir, caminho_telemetria, caminho_banco)


def main() -> None:
    dados_dir = Path(os.environ.get("POO_DADOS", "dados"))
    caminho_telemetria = dados_dir / "telemetria.json"
    caminho_banco = dados_dir / "historico.sqlite"

    st.title("Supervisório — Fermentador Cervejeiro Automatizado")
    col_cab1, col_cab2 = st.columns([3, 1])
    with col_cab1:
        st.caption(f"Diretório de integração: {dados_dir} | Fonte: {caminho_telemetria.name}")
    with col_cab2:
        auto_atualizar = st.toggle("Atualização automática (1s)", value=True)

    if auto_atualizar:
        _painel_auto_atualizado(dados_dir, caminho_telemetria, caminho_banco)
    else:
        _desenhar_painel(dados_dir, caminho_telemetria, caminho_banco)


if __name__ == "__main__":
    main()
