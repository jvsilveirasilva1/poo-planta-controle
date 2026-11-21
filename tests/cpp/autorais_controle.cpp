#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "aquisicao.hpp"
#include "comando.hpp"
#include "controlador.hpp"
#include "politicas.hpp"
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

fermentador::AquisicaoPlanta criarAquisicao(const std::string& sessao,
                                            int ciclo,
                                            double nivel,
                                            double temp,
                                            int ultimo_cmd = 0,
                                            std::optional<std::string> motivo_seguro = std::nullopt) {
    std::vector<fermentador::LeituraSensor> leituras = {
        fermentador::LeituraSensor("LT-01", "nivel", nivel, "%", "operando"),
        fermentador::LeituraSensor("TT-01", "temperatura", temp, "C", "operando")
    };
    return fermentador::AquisicaoPlanta(
        1, sessao, ciclo, "2026-10-21T18:00:00Z",
        std::move(leituras), 0.0, 0.0, ultimo_cmd, std::nullopt, std::move(motivo_seguro));
}

void testarDuasPoliticasComMesmaEntradaEDecisoesDiferentes() {
    std::unique_ptr<fermentador::IPoliticaControle> histerese =
        std::make_unique<fermentador::PoliticaHisterese>(55.0, 65.0, 33.0, 37.0, 0.0, 0.0);
    std::unique_ptr<fermentador::IPoliticaControle> proporcional =
        std::make_unique<fermentador::PoliticaProporcional>(60.0, 35.0, 4.0, 8.0, 33.33, 40.0);

    // Entrada conhecida dentro da banda morta da histerese, mas abaixo do setpoint proporcional
    const auto entrada = criarAquisicao("s1", 1, 58.0, 34.0);

    const auto dec_hist = histerese->calcular(entrada);
    const auto dec_prop = proporcional->calcular(entrada);

    verificar(dec_hist.bomba == 0.0 && dec_hist.aquecedor == 0.0,
              "PoliticaHisterese deve manter 0% dentro da banda morta [55..65] e [33..37]");
    verificar(std::abs(dec_prop.bomba - 41.33) < 1e-2 && std::abs(dec_prop.aquecedor - 48.0) < 1e-2,
              "PoliticaProporcional deve aplicar acao graduada (41.33% e 48.0%)");
    verificar(dec_hist.bomba != dec_prop.bomba && dec_hist.aquecedor != dec_prop.aquecedor,
              "as duas politicas devem tomar decisoes comprovadamente distintas");
}

void testarFronteirasHistereseEPreservacaoDeEstado() {
    fermentador::PoliticaHisterese hist(55.0, 65.0, 33.0, 37.0, 0.0, 0.0);

    // Fronteira inferior exata (55.0% e 33.0C): preserva 0%
    auto d1 = hist.calcular(criarAquisicao("s1", 1, 55.0, 33.0));
    verificar(d1.bomba == 0.0 && d1.aquecedor == 0.0,
              "fronteira inferior exata deve preservar estado desligado");

    // Abaixo da fronteira inferior (54.9% e 32.9C): liga em 100%
    auto d2 = hist.calcular(criarAquisicao("s1", 2, 54.9, 32.9));
    verificar(d2.bomba == 100.0 && d2.aquecedor == 100.0,
              "abaixo da fronteira inferior deve ligar atuadores em 100%");

    // Dentro da banda e na fronteira superior exata (65.0% e 37.0C): preserva 100%
    auto d3 = hist.calcular(criarAquisicao("s1", 3, 65.0, 37.0));
    verificar(d3.bomba == 100.0 && d3.aquecedor == 100.0,
              "fronteira superior exata deve preservar estado ligado (100%)");

    // Acima da fronteira superior (65.1% e 37.1C): desliga para 0%
    auto d4 = hist.calcular(criarAquisicao("s1", 4, 65.1, 37.1));
    verificar(d4.bomba == 0.0 && d4.aquecedor == 0.0,
              "acima da fronteira superior deve desligar atuadores para 0%");

    // Falha ao definir estado inválido deve preservar estado anterior
    hist.definirEstado(100.0, 100.0);
    verificarExcecao<std::invalid_argument>(
        [&]() { hist.definirEstado(-10.0, 50.0); },
        "estado negativo na histerese deve ser rejeitado");
    verificar(hist.estadoBomba() == 100.0 && hist.estadoAquecedor() == 100.0,
              "rejeicao em definirEstado deve preservar estado 100%");

    // Construtor inválido
    verificarExcecao<std::invalid_argument>(
        []() { fermentador::PoliticaHisterese(70.0, 50.0, 33.0, 37.0); },
        "faixa de nivel invertida deve lancar excecao");
}

void testarPoliticaProporcionalSaturacaoEFalhas() {
    fermentador::PoliticaProporcional prop(60.0, 35.0, 4.0, 8.0, 33.33, 40.0);

    // No setpoint exato: retorna exatamente o bias de equilíbrio
    auto d_eq = prop.calcular(criarAquisicao("s1", 1, 60.0, 35.0));
    verificar(std::abs(d_eq.bomba - 33.33) < 1e-2 && std::abs(d_eq.aquecedor - 40.0) < 1e-2,
              "no setpoint deve retornar o bias de equilibrio");

    // Saturação superior (100%) e inferior (0%)
    auto d_max = prop.calcular(criarAquisicao("s1", 2, 0.0, 0.0));
    verificar(d_max.bomba == 100.0 && d_max.aquecedor == 100.0,
              "erro positivo grande deve saturar em 100%");

    auto d_min = prop.calcular(criarAquisicao("s1", 3, 100.0, 100.0));
    verificar(d_min.bomba == 0.0 && d_min.aquecedor == 0.0,
              "erro negativo grande deve saturar em 0%");

    verificarExcecao<std::invalid_argument>(
        []() { fermentador::PoliticaProporcional(60.0, 35.0, -1.0, 8.0); },
        "ganho proporcional negativo deve ser rejeitado");
}

void testarPadraoCommandCicloDeVidaEReinicioSessao() {
    // Validações de invariantes de ComandoAtuacao
    fermentador::ComandoAtuacao cmd("sessao-1", 5, 75.0, 40.0);
    verificar(cmd.id() == 5 && cmd.sessao() == "sessao-1", "identidade do comando incorreta");

    verificarExcecao<std::invalid_argument>(
        []() { fermentador::ComandoAtuacao("", 1, 50.0, 50.0); },
        "sessao vazia no comando deve falhar");
    verificarExcecao<std::invalid_argument>(
        []() { fermentador::ComandoAtuacao("s1", 0, 50.0, 50.0); },
        "id zero no comando deve falhar");
    verificarExcecao<std::invalid_argument>(
        []() { fermentador::ComandoAtuacao("s1", 1, 105.0, 50.0); },
        "atuador acima de 100 no comando deve falhar");

    // GestorComandos: emissão, repetição de ciclo, confirmação, expiração e reinício de sessão
    fermentador::GestorComandos gestor;
    const auto aq_c1 = criarAquisicao("sessao-A", 1, 50.0, 25.0, 0);
    verificar(gestor.reconciliarCiclo(aq_c1), "ciclo 1 inedito deve permitir emissao");
    const auto emitido_1 = gestor.emitirComando(aq_c1, {100.0, 100.0});
    verificar(emitido_1.id() == 1, "primeiro id deve ser 1");
    verificar(!gestor.ultimoComandoConfirmado(), "ainda nao confirmado antes do proximo ciclo");

    // Releitura do mesmo ciclo 1 NÃO deve permitir novo comando
    verificar(!gestor.reconciliarCiclo(aq_c1), "releitura do ciclo 1 nao deve renovar comando");

    // Ciclo 2 confirma o id=1
    const auto aq_c2 = criarAquisicao("sessao-A", 2, 52.0, 26.5, 1, std::nullopt);
    verificar(gestor.reconciliarCiclo(aq_c2), "ciclo 2 deve ser inedito");
    verificar(gestor.ultimoComandoConfirmado() && gestor.ultimoIdConfirmado() == 1,
              "ciclo 2 deve confirmar o comando id=1");

    // Ciclo 7 sinaliza expiração (watchdog de 5 ciclos da planta)
    const auto aq_exp = criarAquisicao("sessao-A", 7, 48.0, 24.0, 1, "comando_expirado");
    verificar(gestor.reconciliarCiclo(aq_exp), "ciclo 7 deve ser processado");
    verificar(gestor.comandoExpirado(), "deve detectar comando_expirado vindo da planta");

    // Reinício da planta gera nova sessão ("sessao-B", ciclo 1, ultimo_comando=0)
    const auto aq_nova_sessao = criarAquisicao("sessao-B", 1, 50.0, 25.0, 0);
    verificar(gestor.reconciliarCiclo(aq_nova_sessao),
              "reinicio de sessao deve aceitar ciclo 1 da nova sessao");
    const auto emitido_nova = gestor.emitirComando(aq_nova_sessao, {100.0, 100.0});
    verificar(emitido_nova.id() == 1 && emitido_nova.sessao() == "sessao-B",
              "nova sessao deve reiniciar sequencia de ids em 1");
}

void testarControladorIntegradoERecuperacaoDeArquivoInvalido() {
    const auto dir_temp = std::filesystem::temp_directory_path() / "teste_controlador_poo";
    std::filesystem::remove_all(dir_temp);
    std::filesystem::create_directories(dir_temp);

    const std::string json_ciclo1 = R"({
        "versao": 1,
        "sessao": "sessao-int",
        "ciclo": 1,
        "timestamp": "2026-10-21T18:00:00Z",
        "leituras": [
            {"tag": "LT-01", "tipo": "nivel", "valor": 50.0, "unidade": "%", "status": "operando"},
            {"tag": "TT-01", "tipo": "temperatura", "valor": 25.0, "unidade": "C", "status": "operando"}
        ],
        "atuadores": {"bomba": 0.0, "aquecedor": 0.0},
        "ultimo_comando": 0,
        "erro_comando": null,
        "motivo_seguro": "sem_arquivo"
    })";

    fermentador::EmissorTelemetria::gravarArquivoAtomico(dir_temp / "planta.json", json_ciclo1);

    fermentador::Controlador ctrl(std::make_unique<fermentador::PoliticaHisterese>());
    verificar(ctrl.nomePoliticaAtiva() == "Histerese", "politica inicial deve ser Histerese");

    const std::time_t instante =
        fermentador::ValidadorAquisicao::converterTimestampUtc("2026-10-21T18:00:00Z");
    verificar(ctrl.processarCiclo(dir_temp, instante) == 0, "ciclo 1 deve ter sucesso");

    const auto cmd1 = nlohmann::json::parse(
        fermentador::EmissorTelemetria::lerArquivoTexto(dir_temp / "comando_planta.json"));
    verificar(cmd1["id"] == 1 && cmd1["atuadores"]["bomba"] == 100.0,
              "comando do ciclo 1 deve ter id=1 e bomba=100%");

    // Injeção de arquivo inválido: deve retornar código != 0 e preservar telemetria anterior
    fermentador::EmissorTelemetria::gravarArquivoAtomico(dir_temp / "planta.json", "{json_quebrado");
    verificar(ctrl.processarCiclo(dir_temp, instante) != 0,
              "arquivo corrompido deve retornar codigo de erro");
    const auto tel_preservada = nlohmann::json::parse(
        fermentador::EmissorTelemetria::lerArquivoTexto(dir_temp / "telemetria.json"));
    verificar(tel_preservada["ciclo"] == 1, "telemetria do ciclo 1 deve ser preservada apos falha");

    // Recuperação no ciclo 2 + troca dinâmica de política para Proporcional
    ctrl.trocarPolitica(std::make_unique<fermentador::PoliticaProporcional>());
    verificar(ctrl.nomePoliticaAtiva() == "Proporcional", "troca de politica falhou");

    const std::string json_ciclo2 = R"({
        "versao": 1,
        "sessao": "sessao-int",
        "ciclo": 2,
        "timestamp": "2026-10-21T18:00:01Z",
        "leituras": [
            {"tag": "LT-01", "tipo": "nivel", "valor": 58.0, "unidade": "%", "status": "operando"},
            {"tag": "TT-01", "tipo": "temperatura", "valor": 34.0, "unidade": "C", "status": "operando"}
        ],
        "atuadores": {"bomba": 100.0, "aquecedor": 100.0},
        "ultimo_comando": 1,
        "erro_comando": null,
        "motivo_seguro": null
    })";
    fermentador::EmissorTelemetria::gravarArquivoAtomico(dir_temp / "planta.json", json_ciclo2);
    verificar(ctrl.processarCiclo(dir_temp, instante + 1) == 0,
              "controlador deve se recuperar no ciclo 2");

    const auto cmd2 = nlohmann::json::parse(
        fermentador::EmissorTelemetria::lerArquivoTexto(dir_temp / "comando_planta.json"));
    verificar(cmd2["id"] == 2, "ciclo 2 recuperado deve emitir comando id=2");

    std::filesystem::remove_all(dir_temp);
}

}  // namespace

int main() {
    try {
        testarDuasPoliticasComMesmaEntradaEDecisoesDiferentes();
        testarFronteirasHistereseEPreservacaoDeEstado();
        testarPoliticaProporcionalSaturacaoEFalhas();
        testarPadraoCommandCicloDeVidaEReinicioSessao();
        testarControladorIntegradoERecuperacaoDeArquivoInvalido();
        std::cout << "OK: Strategy (Histerese/Proporcional), Command e Controlador verificados\n";
        return 0;
    } catch (const std::exception& erro) {
        std::cerr << "FALHOU: " << erro.what() << '\n';
        return 1;
    }
}
