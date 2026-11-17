#include "controlador.hpp"

#include <iostream>
#include <stdexcept>
#include <utility>

#include "aquisicao.hpp"
#include "telemetria.hpp"

namespace fermentador {

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

int Controlador::processarCiclo(const std::filesystem::path& dir_dados,
                                std::time_t agora_utc) {
    try {
        const auto caminho_planta = dir_dados / "planta.json";
        const auto caminho_comando = dir_dados / "comando_planta.json";
        const auto caminho_telemetria = dir_dados / "telemetria.json";
        const auto caminho_estado = dir_dados / "controlador_estado.json";

        const std::string texto_planta = EmissorTelemetria::lerArquivoTexto(caminho_planta);
        const AquisicaoPlanta aquisicao = ValidadorAquisicao::validarJsonTexto(texto_planta);

        gestor_comandos_.carregarEstado(caminho_estado);
        const bool ciclo_inedito = gestor_comandos_.reconciliarCiclo(aquisicao);

        // Restaura a memória de histerese caso a política ativa seja PoliticaHisterese
        if (auto* histerese = dynamic_cast<PoliticaHisterese*>(politica_.get())) {
            histerese->definirEstado(gestor_comandos_.memoriaBomba(),
                                     gestor_comandos_.memoriaAquecedor());
        }

        if (ciclo_inedito) {
            const DecisaoAtuacao decisao = politica_->calcular(aquisicao);
            const ComandoAtuacao comando = gestor_comandos_.emitirComando(aquisicao, decisao);
            EmissorTelemetria::gravarArquivoAtomico(caminho_comando, comando.paraJson());
            gestor_comandos_.salvarEstado(caminho_estado);
        }

        const bool obsoleta = ValidadorAquisicao::estaObsoleta(
            aquisicao.timestamp(), agora_utc, limite_obsolescencia_seg_);
        const auto alarmes = avaliador_alarmes_.avaliar(aquisicao, obsoleta);

        EmissorTelemetria::publicarTelemetria(caminho_telemetria, aquisicao, alarmes);
        return 0;
    } catch (const std::exception& erro) {
        std::cerr << "Erro no controlador: " << erro.what() << '\n';
        return 1;
    }
}

}  // namespace fermentador
