#include "comando.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

#include "telemetria.hpp"
#include "vendor/nlohmann/json.hpp"

namespace fermentador {

namespace {

bool percentualValido(double valor) {
    return std::isfinite(valor) && valor >= 0.0 && valor <= 100.0;
}

}  // namespace

ComandoAtuacao::ComandoAtuacao(std::string sessao, int id, double bomba, double aquecedor, int versao)
    : versao_(versao),
      sessao_(std::move(sessao)),
      id_(id),
      bomba_(bomba),
      aquecedor_(aquecedor) {
    if (versao_ != 1) {
        throw std::invalid_argument("versao do comando deve ser 1");
    }
    if (sessao_.empty()) {
        throw std::invalid_argument("sessao do comando nao pode ser vazia");
    }
    if (id_ <= 0) {
        throw std::invalid_argument("id do comando deve ser inteiro positivo");
    }
    if (!percentualValido(bomba_) || !percentualValido(aquecedor_)) {
        throw std::invalid_argument("atuadores do comando devem estar em [0, 100]");
    }
}

std::string ComandoAtuacao::paraJson() const {
    nlohmann::json doc;
    doc["versao"] = versao_;
    doc["sessao"] = sessao_;
    doc["id"] = id_;
    doc["atuadores"] = {
        {"bomba", bomba_},
        {"aquecedor", aquecedor_}
    };
    return doc.dump();
}

bool GestorComandos::carregarEstado(const std::filesystem::path& caminho_estado) {
    if (!std::filesystem::exists(caminho_estado)) {
        return false;
    }
    try {
        const std::string texto = EmissorTelemetria::lerArquivoTexto(caminho_estado);
        const auto doc = nlohmann::json::parse(texto);
        if (!doc.is_object()) {
            return false;
        }
        sessao_atual_ = doc.value("sessao", "");
        ultimo_ciclo_processado_ = doc.value("ultimo_ciclo_processado", 0);
        ultimo_id_emitido_ = doc.value("ultimo_id_emitido", 0);
        ultimo_id_confirmado_ = doc.value("ultimo_id_confirmado", 0);
        ultimo_comando_confirmado_ = doc.value("ultimo_comando_confirmado", false);
        comando_expirado_ = doc.value("comando_expirado", false);
        memoria_bomba_ = doc.value("memoria_bomba", 0.0);
        memoria_aquecedor_ = doc.value("memoria_aquecedor", 0.0);
        if (!percentualValido(memoria_bomba_)) {
            memoria_bomba_ = 0.0;
        }
        if (!percentualValido(memoria_aquecedor_)) {
            memoria_aquecedor_ = 0.0;
        }
        return true;
    } catch (...) {
        // Recuperação segura caso controlador_estado.json esteja corrompido
        sessao_atual_.clear();
        ultimo_ciclo_processado_ = 0;
        ultimo_id_emitido_ = 0;
        ultimo_id_confirmado_ = 0;
        ultimo_comando_confirmado_ = false;
        comando_expirado_ = false;
        memoria_bomba_ = 0.0;
        memoria_aquecedor_ = 0.0;
        return false;
    }
}

void GestorComandos::salvarEstado(const std::filesystem::path& caminho_estado) const {
    nlohmann::json doc;
    doc["sessao"] = sessao_atual_;
    doc["ultimo_ciclo_processado"] = ultimo_ciclo_processado_;
    doc["ultimo_id_emitido"] = ultimo_id_emitido_;
    doc["ultimo_id_confirmado"] = ultimo_id_confirmado_;
    doc["ultimo_comando_confirmado"] = ultimo_comando_confirmado_;
    doc["comando_expirado"] = comando_expirado_;
    doc["memoria_bomba"] = memoria_bomba_;
    doc["memoria_aquecedor"] = memoria_aquecedor_;
    EmissorTelemetria::gravarArquivoAtomico(caminho_estado, doc.dump());
}

bool GestorComandos::reconciliarCiclo(const AquisicaoPlanta& aquisicao) {
    // Detecta reinício da planta (nova sessão ou regressão de ciclo)
    if (sessao_atual_ != aquisicao.sessao() || aquisicao.ciclo() < ultimo_ciclo_processado_) {
        sessao_atual_ = aquisicao.sessao();
        ultimo_ciclo_processado_ = 0;
        ultimo_id_emitido_ = 0;
        ultimo_id_confirmado_ = 0;
        ultimo_comando_confirmado_ = false;
        comando_expirado_ = false;
        memoria_bomba_ = aquisicao.bombaAplicada();
        memoria_aquecedor_ = aquisicao.aquecedorAplicado();
    }

    // Reconcilia confirmação do comando anterior pela planta
    if (aquisicao.ultimoComando() > 0) {
        ultimo_id_confirmado_ = aquisicao.ultimoComando();
        if (ultimo_id_emitido_ > 0 && aquisicao.ultimoComando() == ultimo_id_emitido_) {
            ultimo_comando_confirmado_ = true;
        }
    }

    comando_expirado_ = aquisicao.motivoSeguro().has_value() &&
                        aquisicao.motivoSeguro().value() == "comando_expirado";

    // Se o ciclo já foi processado nesta sessão, não renova nem emite novo comando
    if (aquisicao.ciclo() <= ultimo_ciclo_processado_) {
        return false;
    }

    return true;
}

ComandoAtuacao GestorComandos::emitirComando(const AquisicaoPlanta& aquisicao,
                                             const DecisaoAtuacao& decisao) {
    const int proximo_id = std::max(aquisicao.ultimoComando(), ultimo_id_emitido_) + 1;
    ComandoAtuacao cmd(aquisicao.sessao(), proximo_id, decisao.bomba, decisao.aquecedor);

    sessao_atual_ = aquisicao.sessao();
    ultimo_ciclo_processado_ = aquisicao.ciclo();
    ultimo_id_emitido_ = proximo_id;
    ultimo_comando_confirmado_ = false;
    memoria_bomba_ = decisao.bomba;
    memoria_aquecedor_ = decisao.aquecedor;

    return cmd;
}

}  // namespace fermentador
