#pragma once

#include <ctime>
#include <filesystem>
#include <memory>
#include <string>

#include "alarmes.hpp"
#include "comando.hpp"
#include "politicas.hpp"

namespace fermentador {

/**
 * Classe principal do Controlador do Fermentador Cervejeiro.
 * Compõe a política de controle polimórfica (Strategy), o gestor de comandos (Command)
 * e o avaliador de alarmes.
 */
class Controlador {
    std::unique_ptr<IPoliticaControle> politica_;
    GestorComandos gestor_comandos_;
    AvaliadorAlarmes avaliador_alarmes_;
    int limite_obsolescencia_seg_;

public:
    explicit Controlador(
        std::unique_ptr<IPoliticaControle> politica = std::make_unique<PoliticaHisterese>(),
        AvaliadorAlarmes avaliador_alarmes = AvaliadorAlarmes(),
        int limite_obsolescencia_seg = 10);

    void trocarPolitica(std::unique_ptr<IPoliticaControle> nova_politica);
    std::string nomePoliticaAtiva() const;

    /**
     * Executa um ciclo completo de aquisição, decisão de controle, emissão de comando
     * (se o ciclo for inédito) e publicação de telemetria.
     * Retorna 0 em caso de sucesso e 1 em caso de falha de aquisição/validação.
     */
    int processarCiclo(const std::filesystem::path& dir_dados,
                       std::time_t agora_utc = std::time(nullptr));

    const GestorComandos& gestorComandos() const noexcept { return gestor_comandos_; }
};

}  // namespace fermentador
