#pragma once

#include <cmath>
#include <ctime>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace fermentador {

/**
 * Representa uma leitura individual de sensor da planta (ex.: LT-01 ou TT-01).
 * Protege as invariantes de domínio: tag não vazia, tipo/unidade compatíveis,
 * status "operando" e valor finito dentro de [0.0, 100.0].
 */
class LeituraSensor {
    std::string tag_;
    std::string tipo_;
    double valor_;
    std::string unidade_;
    std::string status_;

public:
    LeituraSensor(std::string tag, std::string tipo, double valor,
                  std::string unidade, std::string status);

    void atualizarValor(double novo_valor);

    const std::string& tag() const noexcept { return tag_; }
    const std::string& tipo() const noexcept { return tipo_; }
    double valor() const noexcept { return valor_; }
    const std::string& unidade() const noexcept { return unidade_; }
    const std::string& status() const noexcept { return status_; }
};

/**
 * Representa um snapshot completo e validado adquirido de dados/planta.json.
 */
class AquisicaoPlanta {
    int versao_;
    std::string sessao_;
    int ciclo_;
    std::string timestamp_;
    std::vector<LeituraSensor> leituras_;
    double bomba_aplicada_;
    double aquecedor_aplicado_;
    int ultimo_comando_;
    std::optional<std::string> erro_comando_;
    std::optional<std::string> motivo_seguro_;
    double nivel_;
    double temperatura_;

public:
    AquisicaoPlanta(int versao,
                    std::string sessao,
                    int ciclo,
                    std::string timestamp,
                    std::vector<LeituraSensor> leituras,
                    double bomba_aplicada,
                    double aquecedor_aplicado,
                    int ultimo_comando,
                    std::optional<std::string> erro_comando,
                    std::optional<std::string> motivo_seguro);

    int versao() const noexcept { return versao_; }
    const std::string& sessao() const noexcept { return sessao_; }
    int ciclo() const noexcept { return ciclo_; }
    const std::string& timestamp() const noexcept { return timestamp_; }
    const std::vector<LeituraSensor>& leituras() const noexcept { return leituras_; }
    double bombaAplicada() const noexcept { return bomba_aplicada_; }
    double aquecedorAplicado() const noexcept { return aquecedor_aplicado_; }
    int ultimoComando() const noexcept { return ultimo_comando_; }
    const std::optional<std::string>& erroComando() const noexcept { return erro_comando_; }
    const std::optional<std::string>& motivoSeguro() const noexcept { return motivo_seguro_; }

    double nivel() const noexcept { return nivel_; }
    double temperatura() const noexcept { return temperatura_; }
};

/**
 * Valida o documento JSON bruto da planta e verifica idade/obsolescência do snapshot.
 */
class ValidadorAquisicao {
public:
    static AquisicaoPlanta validarJsonTexto(const std::string& texto_json);
    static bool formatoTimestampUtcValido(const std::string& timestamp_utc);
    static std::time_t converterTimestampUtc(const std::string& timestamp_utc);
    static bool estaObsoleta(const std::string& timestamp_utc,
                             std::time_t agora_utc,
                             int limite_segundos = 5);
};

}  // namespace fermentador
