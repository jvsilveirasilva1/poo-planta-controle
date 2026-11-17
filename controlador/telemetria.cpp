#include "telemetria.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>

#include "vendor/nlohmann/json.hpp"

namespace fermentador {

std::string EmissorTelemetria::lerArquivoTexto(const std::filesystem::path& caminho) {
    if (!std::filesystem::exists(caminho)) {
        throw std::runtime_error("arquivo ausente: " + caminho.string());
    }
    std::ifstream entrada(caminho, std::ios::in | std::ios::binary);
    if (!entrada.is_open()) {
        throw std::runtime_error("nao foi possivel abrir o arquivo: " + caminho.string());
    }
    std::ostringstream buffer;
    buffer << entrada.rdbuf();
    if (entrada.bad()) {
        throw std::runtime_error("erro de leitura no arquivo: " + caminho.string());
    }
    return buffer.str();
}

void EmissorTelemetria::gravarArquivoAtomico(const std::filesystem::path& destino,
                                             const std::string& conteudo) {
    const auto diretorio = destino.parent_path();
    if (!diretorio.empty()) {
        std::filesystem::create_directories(diretorio);
    }
    const std::filesystem::path temporario =
        diretorio / ("." + destino.filename().string() + ".tmp");

    {
        std::ofstream saida(temporario, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!saida.is_open()) {
            throw std::runtime_error("falha ao criar arquivo temporario: " + temporario.string());
        }
        saida << conteudo;
        if (!conteudo.empty() && conteudo.back() != '\n') {
            saida << '\n';
        }
        saida.flush();
        if (!saida.good()) {
            throw std::runtime_error("falha ao gravar dados no arquivo temporario: " +
                                     temporario.string());
        }
    }

    std::error_code ec;
    std::filesystem::rename(temporario, destino, ec);
    if (ec) {
        std::filesystem::remove(temporario);
        throw std::runtime_error("falha ao substituir arquivo destino atomicamente: " +
                                 ec.message());
    }
}

std::string EmissorTelemetria::gerarJsonTelemetria(const AquisicaoPlanta& aquisicao,
                                                   const std::vector<Alarme>& alarmes) {
    nlohmann::json doc;
    doc["versao"] = aquisicao.versao();
    doc["sessao"] = aquisicao.sessao();
    doc["ciclo"] = aquisicao.ciclo();
    doc["timestamp"] = aquisicao.timestamp();

    nlohmann::json lista_leituras = nlohmann::json::array();
    for (const auto& l : aquisicao.leituras()) {
        lista_leituras.push_back({
            {"tag", l.tag()},
            {"tipo", l.tipo()},
            {"valor", l.valor()},
            {"unidade", l.unidade()},
            {"status", l.status()}
        });
    }
    doc["leituras"] = std::move(lista_leituras);

    doc["atuadores"] = {
        {"bomba", aquisicao.bombaAplicada()},
        {"aquecedor", aquisicao.aquecedorAplicado()}
    };
    doc["ultimo_comando"] = aquisicao.ultimoComando();

    if (aquisicao.erroComando().has_value()) {
        doc["erro_comando"] = aquisicao.erroComando().value();
    } else {
        doc["erro_comando"] = nullptr;
    }

    if (aquisicao.motivoSeguro().has_value()) {
        doc["motivo_seguro"] = aquisicao.motivoSeguro().value();
    } else {
        doc["motivo_seguro"] = nullptr;
    }

    nlohmann::json lista_alarmes = nlohmann::json::array();
    for (const auto& alarme : alarmes) {
        lista_alarmes.push_back({
            {"codigo", alarme.codigo},
            {"mensagem", alarme.mensagem},
            {"severidade", alarme.severidade}
        });
    }
    doc["alarmes"] = std::move(lista_alarmes);

    return doc.dump();
}

void EmissorTelemetria::publicarTelemetria(const std::filesystem::path& caminho_telemetria,
                                           const AquisicaoPlanta& aquisicao,
                                           const std::vector<Alarme>& alarmes) {
    const std::string conteudo = gerarJsonTelemetria(aquisicao, alarmes);
    gravarArquivoAtomico(caminho_telemetria, conteudo);
}

}  // namespace fermentador
