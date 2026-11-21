#pragma once

#include <string>

#include "aquisicao.hpp"

namespace fermentador {

struct DecisaoAtuacao {
    double bomba;
    double aquecedor;
};

/**
 * Interface Strategy para políticas de controle do Fermentador Cervejeiro.
 * Permite substituir a lei de controle polimorficamente mantendo a aquisição
 * e o transporte desacoplados.
 */
class IPoliticaControle {
public:
    virtual ~IPoliticaControle() = default;
    virtual DecisaoAtuacao calcular(const AquisicaoPlanta& aquisicao) = 0;
    virtual std::string nome() const = 0;
};

/**
 * Estratégia 1 (Principal / Padrão): Controle On-Off com Banda Morta (Histerese).
 * Preserva o estado anterior do atuador enquanto a grandeza permanecer dentro da
 * faixa [minimo, maximo], evitando chaveamento por ruído.
 */
class PoliticaHisterese : public IPoliticaControle {
    double nivel_min_;
    double nivel_max_;
    double temp_min_;
    double temp_max_;
    double estado_bomba_;
    double estado_aquecedor_;

public:
    PoliticaHisterese(double nivel_min = 55.0,
                      double nivel_max = 65.0,
                      double temp_min = 33.0,
                      double temp_max = 37.0,
                      double estado_inicial_bomba = 0.0,
                      double estado_inicial_aquecedor = 0.0);

    DecisaoAtuacao calcular(const AquisicaoPlanta& aquisicao) override;
    std::string nome() const override { return "Histerese"; }

    void definirEstado(double bomba, double aquecedor);

    double estadoBomba() const noexcept { return estado_bomba_; }
    double estadoAquecedor() const noexcept { return estado_aquecedor_; }
    double nivelMin() const noexcept { return nivel_min_; }
    double nivelMax() const noexcept { return nivel_max_; }
    double tempMin() const noexcept { return temp_min_; }
    double tempMax() const noexcept { return temp_max_; }
};

/**
 * Estratégia 2 (Comparativa): Controle Proporcional Contínuo com Bias de Equilíbrio.
 * Modula continuamente os atuadores em [0, 100] proporcionalmente ao erro em relação
 * ao setpoint, somado ao bias de equilíbrio físico da planta (33.33% e 40.0%).
 */
class PoliticaProporcional : public IPoliticaControle {
    double setpoint_nivel_;
    double setpoint_temp_;
    double kp_nivel_;
    double kp_temp_;
    double bias_bomba_;
    double bias_aquecedor_;

public:
    PoliticaProporcional(double setpoint_nivel = 60.0,
                         double setpoint_temp = 35.0,
                         double kp_nivel = 4.0,
                         double kp_temp = 8.0,
                         double bias_bomba = 33.33,
                         double bias_aquecedor = 40.0);

    DecisaoAtuacao calcular(const AquisicaoPlanta& aquisicao) override;
    std::string nome() const override { return "Proporcional"; }

    double setpointNivel() const noexcept { return setpoint_nivel_; }
    double setpointTemp() const noexcept { return setpoint_temp_; }
    double kpNivel() const noexcept { return kp_nivel_; }
    double kpTemp() const noexcept { return kp_temp_; }
};

}  // namespace fermentador
