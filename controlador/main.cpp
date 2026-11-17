#include <chrono>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#include "alarmes.hpp"
#include "aquisicao.hpp"
#include "telemetria.hpp"

namespace {

int processarUmCiclo(const std::filesystem::path& dir_dados) {
    try {
        const auto caminho_planta = dir_dados / "planta.json";
        const auto caminho_telemetria = dir_dados / "telemetria.json";

        const std::string texto_planta =
            fermentador::EmissorTelemetria::lerArquivoTexto(caminho_planta);
        const fermentador::AquisicaoPlanta aquisicao =
            fermentador::ValidadorAquisicao::validarJsonTexto(texto_planta);

        const std::time_t agora_utc = std::time(nullptr);
        const bool obsoleta = fermentador::ValidadorAquisicao::estaObsoleta(
            aquisicao.timestamp(), agora_utc, 10);

        const fermentador::AvaliadorAlarmes avaliador;
        const auto alarmes = avaliador.avaliar(aquisicao, obsoleta);

        fermentador::EmissorTelemetria::publicarTelemetria(
            caminho_telemetria, aquisicao, alarmes);

        return 0;
    } catch (const std::exception& erro) {
        std::cerr << "Erro na aquisicao: " << erro.what() << '\n';
        return 1;
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    std::filesystem::path dir_dados = "dados";
    bool uma_vez = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--ajuda" || arg == "--help") {
            std::cout << "Uso: controlador [--dados DIRETORIO] [--uma-vez]\n";
            return 0;
        } else if (arg == "--dados" && i + 1 < argc) {
            dir_dados = argv[++i];
        } else if (arg == "--uma-vez") {
            uma_vez = true;
        } else {
            std::cerr << "Argumento desconhecido: " << arg << '\n';
            return 2;
        }
    }

    if (uma_vez) {
        return processarUmCiclo(dir_dados);
    }

    while (true) {
        processarUmCiclo(dir_dados);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}
