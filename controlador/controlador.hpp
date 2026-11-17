#pragma once

#include <ctime>
#include <filesystem>
#include <memory>
#include <string>

#include "alarmes.hpp"
#include "comando.hpp"
#include "politicas.hpp"
#include "transporte_tcp.hpp"

namespace fermentador {

/**
 * Classe principal do Controlador do Fermentador Cervejeiro.
 * Compõe a política de controle polimórfica (Strategy), o gestor de comandos (Command)
 * e o avaliador de alarmes, operando tanto por arquivos locais quanto via TCP (Bônus).
 */
class Controlador {
    std::unique_ptr<IPoliticaControle> politica_;
    GestorComandos gestor_comandos_;
    AvaliadorAlarmes avaliador_alarmes_;
    int limite_obsolescencia_seg_;

    int processarTextoAquisicao(const std::string& texto_planta,
                                const std::filesystem::path& dir_dados,
                                std::time_t agora_utc,
                                ClienteTcpPlanta* cliente_tcp = nullptr);

public:
    explicit Controlador(
        std::unique_ptr<IPoliticaControle> politica = std::make_unique<PoliticaHisterese>(),
        AvaliadorAlarmes avaliador_alarmes = AvaliadorAlarmes(),
        int limite_obsolescencia_seg = 10);

    void trocarPolitica(std::unique_ptr<IPoliticaControle> nova_politica);
    std::string nomePoliticaAtiva() const;

    /**
     * Executa um ciclo completo por arquivos locais (dados/planta.json -> comando_planta.json + telemetria.json).
     */
    int processarCiclo(const std::filesystem::path& dir_dados,
                       std::time_t agora_utc = std::time(nullptr));

    /**
     * Executa um ciclo completo adquirindo a planta e enviando o comando via socket TCP (Bônus),
     * preservando os contratos, sessão/ciclo e telemetria.
     */
    int processarCicloTcp(ClienteTcpPlanta& cliente_tcp,
                          const std::filesystem::path& dir_dados,
                          std::time_t agora_utc = std::time(nullptr));

    const GestorComandos& gestorComandos() const noexcept { return gestor_comandos_; }
};

}  // namespace fermentador
