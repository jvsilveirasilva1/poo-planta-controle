#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "alarmes.hpp"
#include "aquisicao.hpp"

namespace fermentador {

/**
 * Utilitário de persistência atômica em arquivos locais (.tmp + substituição por rename)
 * e geração do documento telemetria.json conforme docs/CONTRATO.md.
 */
class EmissorTelemetria {
public:
    static std::string lerArquivoTexto(const std::filesystem::path& caminho);
    static void gravarArquivoAtomico(const std::filesystem::path& destino,
                                     const std::string& conteudo);

    static std::string gerarJsonTelemetria(const AquisicaoPlanta& aquisicao,
                                           const std::vector<Alarme>& alarmes);

    static void publicarTelemetria(const std::filesystem::path& caminho_telemetria,
                                   const AquisicaoPlanta& aquisicao,
                                   const std::vector<Alarme>& alarmes);
};

}  // namespace fermentador
