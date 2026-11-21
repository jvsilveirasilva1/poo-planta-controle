#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "controlador.hpp"
#include "politicas.hpp"
#include "transporte_tcp.hpp"

namespace {

bool separarHostPorta(const std::string& endereco, std::string& host, int& porta) {
    const auto pos = endereco.rfind(':');
    if (pos == std::string::npos || pos == 0 || pos + 1 >= endereco.size()) {
        return false;
    }
    host = endereco.substr(0, pos);
    try {
        porta = std::stoi(endereco.substr(pos + 1));
        return porta > 0 && porta <= 65535;
    } catch (...) {
        return false;
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    std::filesystem::path dir_dados = "dados";
    bool uma_vez = false;
    std::string nome_politica = "histerese";
    std::string endereco_tcp;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--ajuda" || arg == "--help") {
            std::cout << "Uso: controlador [--dados DIRETORIO] [--uma-vez] "
                         "[--politica histerese|proporcional] [--tcp HOST:PORTA]\n";
            return 0;
        } else if (arg == "--dados" && i + 1 < argc) {
            dir_dados = argv[++i];
        } else if (arg == "--politica" && i + 1 < argc) {
            nome_politica = argv[++i];
        } else if (arg == "--tcp" && i + 1 < argc) {
            endereco_tcp = argv[++i];
        } else if (arg == "--uma-vez") {
            uma_vez = true;
        } else {
            std::cerr << "Argumento desconhecido: " << arg << '\n';
            return 2;
        }
    }

    std::unique_ptr<fermentador::IPoliticaControle> politica;
    if (nome_politica == "histerese") {
        politica = std::make_unique<fermentador::PoliticaHisterese>();
    } else if (nome_politica == "proporcional") {
        politica = std::make_unique<fermentador::PoliticaProporcional>();
    } else {
        std::cerr << "Politica desconhecida: " << nome_politica << '\n';
        return 2;
    }

    fermentador::Controlador controlador(std::move(politica));

    if (!endereco_tcp.empty()) {
        std::string host;
        int porta = 0;
        if (!separarHostPorta(endereco_tcp, host, porta)) {
            std::cerr << "Endereco TCP invalido (esperado HOST:PORTA): " << endereco_tcp << '\n';
            return 2;
        }
        fermentador::ClienteTcpPlanta cliente_tcp(host, porta);
        if (uma_vez) {
            return controlador.processarCicloTcp(cliente_tcp, dir_dados);
        }
        while (true) {
            controlador.processarCicloTcp(cliente_tcp, dir_dados);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    if (uma_vez) {
        return controlador.processarCiclo(dir_dados);
    }

    while (true) {
        controlador.processarCiclo(dir_dados);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}
