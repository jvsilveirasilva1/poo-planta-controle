#include "politicas.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace fermentador {

namespace {

bool faixaValida(double minimo, double maximo) {
    return std::isfinite(minimo) && std::isfinite(maximo) &&
           minimo >= 0.0 && maximo <= 100.0 && minimo < maximo;
}

bool percentualValido(double valor) {
    return std::isfinite(valor) && valor >= 0.0 && valor <= 100.0;
}

double arredondarDuasCasas(double valor) {
    return std::round(valor * 100.0) / 100.0;
}

}  // namespace

PoliticaHisterese::PoliticaHisterese(double nivel_min,
                                     double nivel_max,
                                     double temp_min,
                                     double temp_max,
                                     double estado_inicial_bomba,
                                     double estado_inicial_aquecedor)
    : nivel_min_(nivel_min),
      nivel_max_(nivel_max),
      temp_min_(temp_min),
      temp_max_(temp_max),
      estado_bomba_(0.0),
      estado_aquecedor_(0.0) {
    if (!faixaValida(nivel_min_, nivel_max_)) {
        throw std::invalid_argument("faixa de histerese de nivel invalida");
    }
    if (!faixaValida(temp_min_, temp_max_)) {
        throw std::invalid_argument("faixa de histerese de temperatura invalida");
    }
    definirEstado(estado_inicial_bomba, estado_inicial_aquecedor);
}

void PoliticaHisterese::definirEstado(double bomba, double aquecedor) {
    if (!percentualValido(bomba) || !percentualValido(aquecedor)) {
        throw std::invalid_argument("estado inicial dos atuadores deve estar em [0, 100]");
    }
    estado_bomba_ = bomba;
    estado_aquecedor_ = aquecedor;
}

DecisaoAtuacao PoliticaHisterese::calcular(const AquisicaoPlanta& aquisicao) {
    const double nivel = aquisicao.nivel();
    const double temp = aquisicao.temperatura();

    if (nivel < nivel_min_) {
        estado_bomba_ = 100.0;
    } else if (nivel > nivel_max_) {
        estado_bomba_ = 0.0;
    }
    // Se nivel_min_ <= nivel <= nivel_max_, preserva estado_bomba_

    if (temp < temp_min_) {
        estado_aquecedor_ = 100.0;
    } else if (temp > temp_max_) {
        estado_aquecedor_ = 0.0;
    }
    // Se temp_min_ <= temp <= temp_max_, preserva estado_aquecedor_

    return DecisaoAtuacao{estado_bomba_, estado_aquecedor_};
}

PoliticaProporcional::PoliticaProporcional(double setpoint_nivel,
                                           double setpoint_temp,
                                           double kp_nivel,
                                           double kp_temp,
                                           double bias_bomba,
                                           double bias_aquecedor)
    : setpoint_nivel_(setpoint_nivel),
      setpoint_temp_(setpoint_temp),
      kp_nivel_(kp_nivel),
      kp_temp_(kp_temp),
      bias_bomba_(bias_bomba),
      bias_aquecedor_(bias_aquecedor) {
    if (!percentualValido(setpoint_nivel_) || !percentualValido(setpoint_temp_)) {
        throw std::invalid_argument("setpoints devem estar em [0, 100]");
    }
    if (!std::isfinite(kp_nivel_) || kp_nivel_ <= 0.0 ||
        !std::isfinite(kp_temp_) || kp_temp_ <= 0.0) {
        throw std::invalid_argument("ganhos proporcionais devem ser positivos e finitos");
    }
    if (!percentualValido(bias_bomba_) || !percentualValido(bias_aquecedor_)) {
        throw std::invalid_argument("bias dos atuadores deve estar em [0, 100]");
    }
}

DecisaoAtuacao PoliticaProporcional::calcular(const AquisicaoPlanta& aquisicao) {
    const double erro_nivel = setpoint_nivel_ - aquisicao.nivel();
    const double erro_temp = setpoint_temp_ - aquisicao.temperatura();

    const double bomba_bruta = bias_bomba_ + kp_nivel_ * erro_nivel;
    const double aquecedor_bruto = bias_aquecedor_ + kp_temp_ * erro_temp;

    const double bomba = arredondarDuasCasas(std::clamp(bomba_bruta, 0.0, 100.0));
    const double aquecedor = arredondarDuasCasas(std::clamp(aquecedor_bruto, 0.0, 100.0));

    return DecisaoAtuacao{bomba, aquecedor};
}

}  // namespace fermentador
