#include "controlador.hpp"

#include <iostream>
#include <stdexcept>
#include <utility>

#include "aquisicao.hpp"
#include "telemetria.hpp"
#include "vendor/nlohmann/json.hpp"

namespace fermentador {

namespace {

std::string lerModoOperador(const std::filesystem::path& caminho_operador) {
    if (!std::filesystem::exists(caminho_operador)) {
        return "normal";
    }
    try {
        const std::string texto = EmissorTelemetria::lerArquivoTexto(caminho_operador);
        const auto doc = nlohmann::json::parse(texto);
        if (!doc.is_object()) {
            return "normal";
        }
        if (!doc.contains("versao") || !doc["versao"].is_number_integer() ||
            doc["versao"].is_boolean() || doc["versao"].get<int>() != 1) {
            return "normal";
        }
        if (!doc.contains("modo") || !doc["modo"].is_string()) {
            return "normal";
        }
        const std::string modo = doc["modo"].get<std::string>();
        if (modo == "falha_bomba" || modo == "sobrecarga_termica" ||
            modo == "pausar_comandos" || modo == "normal") {
            return modo;
        }
    } catch (...) {
        // Arquivo de operador malformado é ignorado com segurança
    }
    return "normal";
}

}  // namespace

Controlador::Controlador(std::unique_ptr<IPoliticaControle> politica,
                         AvaliadorAlarmes avaliador_alarmes,
                         int limite_obsolescencia_seg)
    : politica_(std::move(politica)),
      gestor_comandos_(),
      avaliador_alarmes_(std::move(avaliador_alarmes)),
      limite_obsolescencia_seg_(limite_obsolescencia_seg) {
    if (!politica_) {
        throw std::invalid_argument("politica de controle nao pode ser nula");
    }
    if (limite_obsolescencia_seg_ <= 0) {
        throw std::invalid_argument("limite de obsolescencia deve ser positivo");
    }
}

void Controlador::trocarPolitica(std::unique_ptr<IPoliticaControle> nova_politica) {
    if (!nova_politica) {
        throw std::invalid_argument("nova politica de controle nao pode ser nula");
    }
    politica_ = std::move(nova_politica);
}

std::string Controlador::nomePoliticaAtiva() const {
    return politica_->nome();
}

int Controlador::processarTextoAquisicao(const std::string& texto_planta,
                                         const std::filesystem::path& dir_dados,
                                         std::time_t agora_utc,
                                         ClienteTcpPlanta* cliente_tcp) {
    const auto caminho_comando = dir_dados / "comando_planta.json";
    const auto caminho_telemetria = dir_dados / "telemetria.json";
    const auto caminho_estado = dir_dados / "controlador_estado.json";
    const auto caminho_operador = dir_dados / "operador_comando.json";

    const AquisicaoPlanta aquisicao = ValidadorAquisicao::validarJsonTexto(texto_planta);

    gestor_comandos_.carregarEstado(caminho_estado);
    const bool ciclo_inedito = gestor_comandos_.reconciliarCiclo(aquisicao);

    if (auto* histerese = dynamic_cast<PoliticaHisterese*>(politica_.get())) {
        histerese->definirEstado(gestor_comandos_.memoriaBomba(),
                                 gestor_comandos_.memoriaAquecedor());
    }

    if (ciclo_inedito) {
        const std::string modo_operador = lerModoOperador(caminho_operador);
        if (modo_operador != "pausar_comandos") {
            DecisaoAtuacao decisao = politica_->calcular(aquisicao);
            if (modo_operador == "falha_bomba") {
                decisao = DecisaoAtuacao{0.0, 0.0};
            } else if (modo_operador == "sobrecarga_termica") {
                decisao = DecisaoAtuacao{100.0, 100.0};
            }

            const ComandoAtuacao comando = gestor_comandos_.emitirComando(aquisicao, decisao);
            const std::string json_cmd = comando.paraJson();

            if (cliente_tcp != nullptr) {
                if (!cliente_tcp->enviarComandoPlanta(json_cmd)) {
                    throw std::runtime_error("falha ao enviar comando de atuacao via TCP");
                }
            }
            EmissorTelemetria::gravarArquivoAtomico(caminho_comando, json_cmd);
            gestor_comandos_.salvarEstado(caminho_estado);
        }
    }

    const bool obsoleta = ValidadorAquisicao::estaObsoleta(
        aquisicao.timestamp(), agora_utc, limite_obsolescencia_seg_);
    const auto alarmes = avaliador_alarmes_.avaliar(aquisicao, obsoleta);

    EmissorTelemetria::publicarTelemetria(caminho_telemetria, aquisicao, alarmes);
    return 0;
}

int Controlador::processarCiclo(const std::filesystem::path& dir_dados,
                                std::time_t agora_utc) {
    try {
        const auto caminho_planta = dir_dados / "planta.json";
        const std::string texto_planta = EmissorTelemetria::lerArquivoTexto(caminho_planta);
        return processarTextoAquisicao(texto_planta, dir_dados, agora_utc, nullptr);
    } catch (const std::exception& erro) {
        std::cerr << "Erro no controlador: " << erro.what() << '\n';
        return 1;
    }
}

int Controlador::processarCicloTcp(ClienteTcpPlanta& cliente_tcp,
                                   const std::filesystem::path& dir_dados,
                                   std::time_t agora_utc) {
    try {
        const auto snapshot = cliente_tcp.requisitarSnapshotPlanta();
        if (!snapshot.has_value()) {
            throw std::runtime_error("servidor TCP da planta indisponivel ou desconectado");
        }
        return processarTextoAquisicao(snapshot.value(), dir_dados, agora_utc, &cliente_tcp);
    } catch (const std::exception& erro) {
        std::cerr << "Erro no controlador TCP: " << erro.what() << '\n';
        return 1;
    }
}

}  // namespace fermentador
