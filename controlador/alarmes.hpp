#pragma once

#include <string>
#include <vector>

#include "aquisicao.hpp"

namespace fermentador {

struct Alarme {
    std::string codigo;
    std::string mensagem;
    std::string severidade;
};

/**
 * Avalia os alarmes de domínio do Fermentador Cervejeiro a partir de uma aquisição validada.
 * Fronteiras padrão:
 * - NIVEL_BAIXO: nivel < 25.0% ("aviso")
 * - NIVEL_ALTO:  nivel > 85.0% ("critico")
 * - TEMP_BAIXA:  temperatura < 22.0 C ("aviso")
 * - TEMP_ALTA:   temperatura > 45.0 C ("critico")
 */
class AvaliadorAlarmes {
    double nivel_baixo_;
    double nivel_alto_;
    double temp_baixa_;
    double temp_alta_;

public:
    AvaliadorAlarmes(double nivel_baixo = 25.0,
                     double nivel_alto = 85.0,
                     double temp_baixa = 22.0,
                     double temp_alta = 45.0);

    std::vector<Alarme> avaliar(const AquisicaoPlanta& aquisicao,
                                bool aquisicao_obsoleta = false) const;

    double nivelBaixo() const noexcept { return nivel_baixo_; }
    double nivelAlto() const noexcept { return nivel_alto_; }
    double tempBaixa() const noexcept { return temp_baixa_; }
    double tempAlta() const noexcept { return temp_alta_; }
};

}  // namespace fermentador
