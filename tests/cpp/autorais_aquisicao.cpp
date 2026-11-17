#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

#include "alarmes.hpp"
#include "aquisicao.hpp"
#include "telemetria.hpp"
#include "vendor/nlohmann/json.hpp"

namespace {

void verificar(bool condicao, const std::string& mensagem) {
    if (!condicao) {
        throw std::runtime_error(mensagem);
    }
}

template <typename Excecao, typename Func>
void verificarExcecao(Func&& fn, const std::string& mensagem) {
    bool lancou = false;
    try {
        fn();
    } catch (const Excecao&) {
        lancou = true;
    }
    if (!lancou) {
        throw std::runtime_error(mensagem);
    }
}

void testarLeituraSensorComumFronteiraEFalha() {
    fermentador::LeituraSensor leitura("LT-01", "nivel", 60.0, "%", "operando");
    verificar(leitura.tag() == "LT-01", "tag inicial incorreta");
    verificar(leitura.tipo() == "nivel", "tipo inicial incorreto");
    verificar(leitura.valor() == 60.0, "valor inicial incorreto");
    verificar(leitura.unidade() == "%", "unidade inicial incorreta");
    verificar(leitura.status() == "operando", "status inicial incorreto");

    // Fronteiras 0.0 e 100.0
    leitura.atualizarValor(0.0);
    verificar(leitura.valor() == 0.0, "fronteira inferior 0.0 deve ser aceita");
    leitura.atualizarValor(100.0);
    verificar(leitura.valor() == 100.0, "fronteira superior 100.0 deve ser aceita");

    // Falha e preservação de estado
    verificarExcecao<std::invalid_argument>(
        [&]() { leitura.atualizarValor(-0.01); },
        "valor abaixo de 0 deve ser rejeitado");
    verificar(leitura.valor() == 100.0, "rejeicao de valor negativo deve preservar estado 100.0");

    verificarExcecao<std::invalid_argument>(
        [&]() { leitura.atualizarValor(100.01); },
        "valor acima de 100 deve ser rejeitado");
    verificar(leitura.valor() == 100.0, "rejeicao acima de 100 deve preservar estado 100.0");

    verificarExcecao<std::invalid_argument>(
        [&]() { leitura.atualizarValor(std::numeric_limits<double>::quiet_NaN()); },
        "NaN deve ser rejeitado");
    verificar(leitura.valor() == 100.0, "rejeicao de NaN deve preservar estado anterior");

    // Falhas de invariantes no construtor
    verificarExcecao<std::invalid_argument>(
        []() { fermentador::LeituraSensor("", "nivel", 50.0, "%", "operando"); },
        "tag vazia deve ser rejeitada");
    verificarExcecao<std::invalid_argument>(
        []() { fermentador::LeituraSensor("LT-01", "nivel", 50.0, "C", "operando"); },
        "unidade 'C' para nivel deve ser rejeitada");
    verificarExcecao<std::invalid_argument>(
        []() { fermentador::LeituraSensor("TT-01", "temperatura", 25.0, "%", "operando"); },
        "unidade '%' para temperatura deve ser rejeitada");
    verificarExcecao<std::invalid_argument>(
        []() { fermentador::LeituraSensor("TT-01", "pressao", 25.0, "bar", "operando"); },
        "tipo desconhecido deve ser rejeitado");
    verificarExcecao<std::invalid_argument>(
        []() { fermentador::LeituraSensor("LT-01", "nivel", 50.0, "%", "falha"); },
        "status 'falha' deve ser rejeitado");
}

void testarValidadorAquisicaoEFronteiraObsolescencia() {
    const std::string json_valido = R"({
        "versao": 1,
        "sessao": "fermentador-s1",
        "ciclo": 4,
        "timestamp": "2026-10-21T18:00:10Z",
        "leituras": [
            {"tag": "LT-01", "tipo": "nivel", "valor": 58.5, "unidade": "%", "status": "operando"},
            {"tag": "TT-01", "tipo": "temperatura", "valor": 34.2, "unidade": "C", "status": "operando"}
        ],
        "atuadores": {"bomba": 33.3, "aquecedor": 40.0},
        "ultimo_comando": 2,
        "erro_comando": null,
        "motivo_seguro": null
    })";

    const auto aq = fermentador::ValidadorAquisicao::validarJsonTexto(json_valido);
    verificar(aq.versao() == 1, "versao deve ser 1");
    verificar(aq.sessao() == "fermentador-s1", "sessao incorreta");
    verificar(aq.ciclo() == 4, "ciclo incorreto");
    verificar(std::abs(aq.nivel() - 58.5) < 1e-6, "nivel extraido incorreto");
    verificar(std::abs(aq.temperatura() - 34.2) < 1e-6, "temperatura extraida incorreta");
    verificar(aq.ultimoComando() == 2, "ultimo_comando incorreto");
    verificar(!aq.erroComando().has_value(), "erro_comando deveria ser nullopt");

    // Teste de fronteira de obsolescência do timestamp UTC
    const std::time_t t_aquisicao =
        fermentador::ValidadorAquisicao::converterTimestampUtc("2026-10-21T18:00:10Z");
    verificar(!fermentador::ValidadorAquisicao::estaObsoleta(
                  "2026-10-21T18:00:10Z", t_aquisicao + 5, 5),
              "idade exatamente igual ao limite (5s) nao deve ser obsoleta");
    verificar(fermentador::ValidadorAquisicao::estaObsoleta(
                  "2026-10-21T18:00:10Z", t_aquisicao + 6, 5),
              "idade acima do limite (6s > 5s) deve ser obsoleta");

    // Rejeição de versao booleana, atuador desconhecido e falta de grandeza
    const std::string json_versao_bool = R"({
        "versao": true,
        "sessao": "s1",
        "ciclo": 1,
        "timestamp": "2026-10-21T18:00:00Z",
        "leituras": [
            {"tag": "LT-01", "tipo": "nivel", "valor": 50.0, "unidade": "%", "status": "operando"},
            {"tag": "TT-01", "tipo": "temperatura", "valor": 25.0, "unidade": "C", "status": "operando"}
        ],
        "atuadores": {"bomba": 0.0, "aquecedor": 0.0},
        "ultimo_comando": 0
    })";
    verificarExcecao<std::invalid_argument>(
        [&]() { fermentador::ValidadorAquisicao::validarJsonTexto(json_versao_bool); },
        "versao booleana deve ser rejeitada");

    const std::string json_sem_temp = R"({
        "versao": 1,
        "sessao": "s1",
        "ciclo": 1,
        "timestamp": "2026-10-21T18:00:00Z",
        "leituras": [
            {"tag": "LT-01", "tipo": "nivel", "valor": 50.0, "unidade": "%", "status": "operando"}
        ],
        "atuadores": {"bomba": 0.0, "aquecedor": 0.0},
        "ultimo_comando": 0
    })";
    verificarExcecao<std::invalid_argument>(
        [&]() { fermentador::ValidadorAquisicao::validarJsonTexto(json_sem_temp); },
        "aquisicao sem a segunda grandeza deve ser rejeitada");
}

void testarAvaliadorAlarmesFronteirasECasosCriticos() {
    fermentador::AvaliadorAlarmes avaliador(25.0, 85.0, 22.0, 45.0);

    auto criarAquisicao = [](double nivel, double temp) {
        std::vector<fermentador::LeituraSensor> leituras = {
            fermentador::LeituraSensor("LT-01", "nivel", nivel, "%", "operando"),
            fermentador::LeituraSensor("TT-01", "temperatura", temp, "C", "operando")
        };
        return fermentador::AquisicaoPlanta(
            1, "sessao-alarme", 1, "2026-10-21T18:00:00Z",
            std::move(leituras), 0.0, 0.0, 0, std::nullopt, std::nullopt);
    };

    // Fronteiras exatas não disparam alarmes
    verificar(avaliador.avaliar(criarAquisicao(25.0, 22.0), false).empty(),
              "fronteiras inferiores exatas (25.0% e 22.0C) nao devem disparar alarme");
    verificar(avaliador.avaliar(criarAquisicao(85.0, 45.0), false).empty(),
              "fronteiras superiores exatas (85.0% e 45.0C) nao devem disparar alarme");

    // Abaixo das fronteiras inferiores
    auto alarmes_baixos = avaliador.avaliar(criarAquisicao(24.9, 21.9), false);
    verificar(alarmes_baixos.size() == 2, "deve disparar NIVEL_BAIXO e TEMP_BAIXA");
    verificar(alarmes_baixos[0].codigo == "NIVEL_BAIXO", "esperado NIVEL_BAIXO");
    verificar(alarmes_baixos[1].codigo == "TEMP_BAIXA", "esperado TEMP_BAIXA");

    // Acima das fronteiras superiores + obsolescência
    auto alarmes_altos = avaliador.avaliar(criarAquisicao(85.1, 45.1), true);
    verificar(alarmes_altos.size() == 3, "deve disparar NIVEL_ALTO, TEMP_ALTA e AQUISICAO_OBSOLETA");
    verificar(alarmes_altos[0].codigo == "NIVEL_ALTO" && alarmes_altos[0].severidade == "critico",
              "esperado NIVEL_ALTO critico");
    verificar(alarmes_altos[1].codigo == "TEMP_ALTA" && alarmes_altos[1].severidade == "critico",
              "esperado TEMP_ALTA critico");
    verificar(alarmes_altos[2].codigo == "AQUISICAO_OBSOLETA",
              "esperado AQUISICAO_OBSOLETA");

    // Construtor com limites invertidos deve falhar
    verificarExcecao<std::invalid_argument>(
        []() { fermentador::AvaliadorAlarmes(85.0, 25.0, 22.0, 45.0); },
        "limites de nivel invertidos devem lancar excecao");
}

void testarEmissorTelemetriaAtomico() {
    const auto dir_temp = std::filesystem::temp_directory_path() / "teste_telemetria_poo";
    std::filesystem::remove_all(dir_temp);
    std::filesystem::create_directories(dir_temp);

    std::vector<fermentador::LeituraSensor> leituras = {
        fermentador::LeituraSensor("LT-01", "nivel", 60.0, "%", "operando"),
        fermentador::LeituraSensor("TT-01", "temperatura", 35.0, "C", "operando")
    };
    fermentador::AquisicaoPlanta aq(
        1, "sessao-tel", 7, "2026-10-21T18:00:00Z",
        std::move(leituras), 33.3, 40.0, 3, std::nullopt, "sem_arquivo");

    const auto caminho_tel = dir_temp / "telemetria.json";
    fermentador::EmissorTelemetria::publicarTelemetria(caminho_tel, aq, {});

    const std::string lido = fermentador::EmissorTelemetria::lerArquivoTexto(caminho_tel);
    const auto doc = nlohmann::json::parse(lido);
    verificar(doc["sessao"] == "sessao-tel", "telemetria perdeu sessao");
    verificar(doc["ciclo"] == 7, "telemetria perdeu ciclo");
    verificar(doc["alarmes"].is_array() && doc["alarmes"].empty(), "alarmes deve ser lista vazia");

    std::filesystem::remove_all(dir_temp);
}

}  // namespace

int main() {
    try {
        testarLeituraSensorComumFronteiraEFalha();
        testarValidadorAquisicaoEFronteiraObsolescencia();
        testarAvaliadorAlarmesFronteirasECasosCriticos();
        testarEmissorTelemetriaAtomico();
        std::cout << "OK: Aquisicao, validacao, alarmes e telemetria verificados com sucesso\n";
        return 0;
    } catch (const std::exception& erro) {
        std::cerr << "FALHOU: " << erro.what() << '\n';
        return 1;
    }
}
