#pragma once

#include <filesystem>
#include <string>

#include "aquisicao.hpp"
#include "politicas.hpp"

namespace fermentador {

/**
 * Padrão Command: encapsula uma solicitação de atuação enviada pelo controlador à planta.
 * Possui identidade própria (sessao + id inteiro crescente) e invariantes de faixa [0, 100].
 */
class ComandoAtuacao {
    int versao_;
    std::string sessao_;
    int id_;
    double bomba_;
    double aquecedor_;

public:
    ComandoAtuacao(std::string sessao, int id, double bomba, double aquecedor, int versao = 1);

    int versao() const noexcept { return versao_; }
    const std::string& sessao() const noexcept { return sessao_; }
    int id() const noexcept { return id_; }
    double bomba() const noexcept { return bomba_; }
    double aquecedor() const noexcept { return aquecedor_; }

    std::string paraJson() const;
};

/**
 * Gerencia o ciclo de vida dos comandos (emissão, idempotência por ciclo,
 * confirmação pela planta, expiração e reinício de sessão), preservando o
 * estado em dados/controlador_estado.json entre execuções --uma-vez.
 */
class GestorComandos {
    std::string sessao_atual_;
    int ultimo_ciclo_processado_ = 0;
    int ultimo_id_emitido_ = 0;
    int ultimo_id_confirmado_ = 0;
    bool ultimo_comando_confirmado_ = false;
    bool comando_expirado_ = false;
    double memoria_bomba_ = 0.0;
    double memoria_aquecedor_ = 0.0;

public:
    GestorComandos() = default;

    bool carregarEstado(const std::filesystem::path& caminho_estado);
    void salvarEstado(const std::filesystem::path& caminho_estado) const;

    /**
     * Reconcilia a aquisição atual com o estado do controlador:
     * - Se a sessão mudou (reinício da planta) ou o ciclo regrediu, reinicia o histórico.
     * - Atualiza o status de confirmação (ultimo_comando == ultimo_id_emitido_) e expiração.
     * Retorna true se este ciclo é inédito e deve gerar um novo ComandoAtuacao.
     */
    bool reconciliarCiclo(const AquisicaoPlanta& aquisicao);

    /**
     * Emite um novo ComandoAtuacao com id estritamente maior que ultimo_comando
     * e registra o ciclo como processado.
     */
    ComandoAtuacao emitirComando(const AquisicaoPlanta& aquisicao,
                                 const DecisaoAtuacao& decisao);

    const std::string& sessaoAtual() const noexcept { return sessao_atual_; }
    int ultimoCicloProcessado() const noexcept { return ultimo_ciclo_processado_; }
    int ultimoIdEmitido() const noexcept { return ultimo_id_emitido_; }
    int ultimoIdConfirmado() const noexcept { return ultimo_id_confirmado_; }
    bool ultimoComandoConfirmado() const noexcept { return ultimo_comando_confirmado_; }
    bool comandoExpirado() const noexcept { return comando_expirado_; }
    double memoriaBomba() const noexcept { return memoria_bomba_; }
    double memoriaAquecedor() const noexcept { return memoria_aquecedor_; }
};

}  // namespace fermentador
