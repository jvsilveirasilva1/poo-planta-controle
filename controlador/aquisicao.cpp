#include "aquisicao.hpp"

#include <cctype>
#include <cmath>
#include <stdexcept>
#include <utility>

#include "vendor/nlohmann/json.hpp"

namespace fermentador {

namespace {

bool numeroFinitoEmFaixa(double valor, double minimo, double maximo) {
    return std::isfinite(valor) && valor >= minimo && valor <= maximo;
}

bool jsonEhInteiroEstrito(const nlohmann::json& j) {
    return j.is_number_integer() && !j.is_boolean();
}

bool jsonEhNumeroEstrito(const nlohmann::json& j) {
    return j.is_number() && !j.is_boolean();
}

int extrairInteiroSubstr(const std::string& s, std::size_t pos, std::size_t tam) {
    int acumulador = 0;
    for (std::size_t i = 0; i < tam; ++i) {
        unsigned char c = static_cast<unsigned char>(s[pos + i]);
        if (!std::isdigit(c)) {
            throw std::invalid_argument("timestamp contem caractere nao numerico");
        }
        acumulador = acumulador * 10 + (s[pos + i] - '0');
    }
    return acumulador;
}

// Algoritmo civil-from-days (Howard Hinnant) para converter AAAA-MM-DD em dias desde 1970-01-01 UTC
long long diasDesdeEpochUtc(int ano, int mes, int dia) {
    ano -= (mes <= 2) ? 1 : 0;
    const int era = (ano >= 0 ? ano : ano - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(ano - era * 400);
    const unsigned doy = (153 * (mes + (mes > 2 ? -3 : 9)) + 2) / 5 + static_cast<unsigned>(dia) - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<long long>(era) * 146097LL + static_cast<long long>(doe) - 719468LL;
}

}  // namespace

LeituraSensor::LeituraSensor(std::string tag, std::string tipo, double valor,
                             std::string unidade, std::string status)
    : tag_(std::move(tag)),
      tipo_(std::move(tipo)),
      valor_(0.0),
      unidade_(std::move(unidade)),
      status_(std::move(status)) {
    if (tag_.empty()) {
        throw std::invalid_argument("tag da leitura nao pode ser vazia");
    }
    if (status_ != "operando") {
        throw std::invalid_argument("status da leitura invalido ou em falha: " + status_);
    }
    if (tipo_ == "nivel") {
        if (unidade_ != "%") {
            throw std::invalid_argument("unidade incompativel para nivel: esperado '%'");
        }
    } else if (tipo_ == "temperatura") {
        if (unidade_ != "C") {
            throw std::invalid_argument("unidade incompativel para temperatura: esperado 'C'");
        }
    } else {
        throw std::invalid_argument("tipo de grandeza desconhecido: " + tipo_);
    }
    atualizarValor(valor);
}

void LeituraSensor::atualizarValor(double novo_valor) {
    if (!numeroFinitoEmFaixa(novo_valor, 0.0, 100.0)) {
        throw std::invalid_argument("valor da leitura fora da faixa [0, 100] ou nao finito");
    }
    valor_ = novo_valor;
}

AquisicaoPlanta::AquisicaoPlanta(int versao,
                                 std::string sessao,
                                 int ciclo,
                                 std::string timestamp,
                                 std::vector<LeituraSensor> leituras,
                                 double bomba_aplicada,
                                 double aquecedor_aplicado,
                                 int ultimo_comando,
                                 std::optional<std::string> erro_comando,
                                 std::optional<std::string> motivo_seguro)
    : versao_(versao),
      sessao_(std::move(sessao)),
      ciclo_(ciclo),
      timestamp_(std::move(timestamp)),
      leituras_(std::move(leituras)),
      bomba_aplicada_(bomba_aplicada),
      aquecedor_aplicado_(aquecedor_aplicado),
      ultimo_comando_(ultimo_comando),
      erro_comando_(std::move(erro_comando)),
      motivo_seguro_(std::move(motivo_seguro)),
      nivel_(0.0),
      temperatura_(0.0) {
    if (versao_ != 1) {
        throw std::invalid_argument("versao do contrato deve ser 1");
    }
    if (sessao_.empty()) {
        throw std::invalid_argument("sessao nao pode ser vazia");
    }
    if (ciclo_ <= 0) {
        throw std::invalid_argument("ciclo deve ser inteiro positivo");
    }
    if (!ValidadorAquisicao::formatoTimestampUtcValido(timestamp_)) {
        throw std::invalid_argument("timestamp deve estar no formato UTC AAAA-MM-DDTHH:MM:SSZ");
    }
    if (!numeroFinitoEmFaixa(bomba_aplicada_, 0.0, 100.0) ||
        !numeroFinitoEmFaixa(aquecedor_aplicado_, 0.0, 100.0)) {
        throw std::invalid_argument("atuadores aplicados devem estar entre 0 e 100");
    }
    if (ultimo_comando_ < 0) {
        throw std::invalid_argument("ultimo_comando nao pode ser negativo");
    }

    bool achou_nivel = false;
    bool achou_temp = false;
    for (const auto& l : leituras_) {
        if (l.tipo() == "nivel") {
            nivel_ = l.valor();
            achou_nivel = true;
        } else if (l.tipo() == "temperatura") {
            temperatura_ = l.valor();
            achou_temp = true;
        }
    }
    if (!achou_nivel || !achou_temp) {
        throw std::invalid_argument("aquisicao exige pelo menos as duas grandezas: nivel e temperatura");
    }
}

bool ValidadorAquisicao::formatoTimestampUtcValido(const std::string& timestamp_utc) {
    if (timestamp_utc.size() != 20) {
        return false;
    }
    if (timestamp_utc[4] != '-' || timestamp_utc[7] != '-' ||
        timestamp_utc[10] != 'T' || timestamp_utc[13] != ':' ||
        timestamp_utc[16] != ':' || timestamp_utc[19] != 'Z') {
        return false;
    }
    try {
        const int ano = extrairInteiroSubstr(timestamp_utc, 0, 4);
        const int mes = extrairInteiroSubstr(timestamp_utc, 5, 2);
        const int dia = extrairInteiroSubstr(timestamp_utc, 8, 2);
        const int hora = extrairInteiroSubstr(timestamp_utc, 11, 2);
        const int min = extrairInteiroSubstr(timestamp_utc, 14, 2);
        const int seg = extrairInteiroSubstr(timestamp_utc, 17, 2);
        if (ano < 1970 || mes < 1 || mes > 12 || dia < 1 || dia > 31 ||
            hora < 0 || hora > 23 || min < 0 || min > 59 || seg < 0 || seg > 60) {
            return false;
        }
    } catch (...) {
        return false;
    }
    return true;
}

std::time_t ValidadorAquisicao::converterTimestampUtc(const std::string& timestamp_utc) {
    if (!formatoTimestampUtcValido(timestamp_utc)) {
        throw std::invalid_argument("formato de timestamp UTC invalido: " + timestamp_utc);
    }
    const int ano = extrairInteiroSubstr(timestamp_utc, 0, 4);
    const int mes = extrairInteiroSubstr(timestamp_utc, 5, 2);
    const int dia = extrairInteiroSubstr(timestamp_utc, 8, 2);
    const int hora = extrairInteiroSubstr(timestamp_utc, 11, 2);
    const int min = extrairInteiroSubstr(timestamp_utc, 14, 2);
    const int seg = extrairInteiroSubstr(timestamp_utc, 17, 2);

    const long long dias = diasDesdeEpochUtc(ano, mes, dia);
    const long long total_segundos = dias * 86400LL + hora * 3600LL + min * 60LL + seg;
    return static_cast<std::time_t>(total_segundos);
}

bool ValidadorAquisicao::estaObsoleta(const std::string& timestamp_utc,
                                      std::time_t agora_utc,
                                      int limite_segundos) {
    if (limite_segundos < 0) {
        throw std::invalid_argument("limite_segundos nao pode ser negativo");
    }
    const std::time_t instante = converterTimestampUtc(timestamp_utc);
    return (agora_utc - instante) > static_cast<std::time_t>(limite_segundos);
}

AquisicaoPlanta ValidadorAquisicao::validarJsonTexto(const std::string& texto_json) {
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(texto_json);
    } catch (const std::exception& e) {
        throw std::invalid_argument(std::string("JSON malformado: ") + e.what());
    }

    if (!doc.is_object()) {
        throw std::invalid_argument("documento raiz deve ser um objeto JSON");
    }

    const char* campos_obrigatorios[] = {
        "versao", "sessao", "ciclo", "timestamp", "leituras", "atuadores", "ultimo_comando"
    };
    for (const char* campo : campos_obrigatorios) {
        if (!doc.contains(campo)) {
            throw std::invalid_argument(std::string("campo obrigatorio ausente: ") + campo);
        }
    }

    if (!jsonEhInteiroEstrito(doc["versao"])) {
        throw std::invalid_argument("campo versao deve ser inteiro estrito");
    }
    const int versao = doc["versao"].get<int>();

    if (!doc["sessao"].is_string()) {
        throw std::invalid_argument("campo sessao deve ser string");
    }
    const std::string sessao = doc["sessao"].get<std::string>();

    if (!jsonEhInteiroEstrito(doc["ciclo"])) {
        throw std::invalid_argument("campo ciclo deve ser inteiro estrito");
    }
    const int ciclo = doc["ciclo"].get<int>();

    if (!doc["timestamp"].is_string()) {
        throw std::invalid_argument("campo timestamp deve ser string");
    }
    const std::string timestamp = doc["timestamp"].get<std::string>();

    if (!doc["leituras"].is_array() || doc["leituras"].empty()) {
        throw std::invalid_argument("campo leituras deve ser uma lista nao vazia");
    }

    std::vector<LeituraSensor> leituras;
    for (const auto& item : doc["leituras"]) {
        if (!item.is_object()) {
            throw std::invalid_argument("cada leitura deve ser um objeto JSON");
        }
        if (!item.contains("tag") || !item["tag"].is_string() ||
            !item.contains("tipo") || !item["tipo"].is_string() ||
            !item.contains("valor") || !jsonEhNumeroEstrito(item["valor"]) ||
            !item.contains("unidade") || !item["unidade"].is_string() ||
            !item.contains("status") || !item["status"].is_string()) {
            throw std::invalid_argument("leitura com campos ausentes ou tipos invalidos");
        }
        leituras.emplace_back(
            item["tag"].get<std::string>(),
            item["tipo"].get<std::string>(),
            item["valor"].get<double>(),
            item["unidade"].get<std::string>(),
            item["status"].get<std::string>());
    }

    const auto& atuadores = doc["atuadores"];
    if (!atuadores.is_object() || atuadores.size() != 2 ||
        !atuadores.contains("bomba") || !atuadores.contains("aquecedor") ||
        !jsonEhNumeroEstrito(atuadores["bomba"]) ||
        !jsonEhNumeroEstrito(atuadores["aquecedor"])) {
        throw std::invalid_argument("campo atuadores deve conter exatamente bomba e aquecedor numericos");
    }
    const double bomba = atuadores["bomba"].get<double>();
    const double aquecedor = atuadores["aquecedor"].get<double>();

    if (!jsonEhInteiroEstrito(doc["ultimo_comando"])) {
        throw std::invalid_argument("campo ultimo_comando deve ser inteiro estrito");
    }
    const int ultimo_comando = doc["ultimo_comando"].get<int>();

    std::optional<std::string> erro_comando = std::nullopt;
    if (doc.contains("erro_comando") && !doc["erro_comando"].is_null()) {
        if (!doc["erro_comando"].is_string()) {
            throw std::invalid_argument("erro_comando deve ser string ou null");
        }
        erro_comando = doc["erro_comando"].get<std::string>();
    }

    std::optional<std::string> motivo_seguro = std::nullopt;
    if (doc.contains("motivo_seguro") && !doc["motivo_seguro"].is_null()) {
        if (!doc["motivo_seguro"].is_string()) {
            throw std::invalid_argument("motivo_seguro deve ser string ou null");
        }
        motivo_seguro = doc["motivo_seguro"].get<std::string>();
    }

    return AquisicaoPlanta(versao, sessao, ciclo, timestamp, std::move(leituras),
                           bomba, aquecedor, ultimo_comando, erro_comando, motivo_seguro);
}

}  // namespace fermentador
