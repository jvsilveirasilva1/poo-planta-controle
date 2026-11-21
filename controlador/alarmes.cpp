#include "alarmes.hpp"

#include <cmath>
#include <stdexcept>

namespace fermentador {

AvaliadorAlarmes::AvaliadorAlarmes(double nivel_baixo,
                                   double nivel_alto,
                                   double temp_baixa,
                                   double temp_alta)
    : nivel_baixo_(nivel_baixo),
      nivel_alto_(nivel_alto),
      temp_baixa_(temp_baixa),
      temp_alta_(temp_alta) {
    if (!std::isfinite(nivel_baixo_) || !std::isfinite(nivel_alto_) ||
        nivel_baixo_ < 0.0 || nivel_alto_ > 100.0 || nivel_baixo_ >= nivel_alto_) {
        throw std::invalid_argument("limites de alarme de nivel invalidos");
    }
    if (!std::isfinite(temp_baixa_) || !std::isfinite(temp_alta_) ||
        temp_baixa_ < 0.0 || temp_alta_ > 100.0 || temp_baixa_ >= temp_alta_) {
        throw std::invalid_argument("limites de alarme de temperatura invalidos");
    }
}

std::vector<Alarme> AvaliadorAlarmes::avaliar(const AquisicaoPlanta& aquisicao,
                                              bool aquisicao_obsoleta) const {
    std::vector<Alarme> alarmes;

    if (aquisicao.nivel() < nivel_baixo_) {
        alarmes.push_back(Alarme{
            "NIVEL_BAIXO",
            "Nivel de mosto no fermentador abaixo do limite minimo seguro",
            "aviso"});
    } else if (aquisicao.nivel() > nivel_alto_) {
        alarmes.push_back(Alarme{
            "NIVEL_ALTO",
            "Nivel de mosto acima do limite maximo (risco de transbordo/blow-off)",
            "critico"});
    }

    if (aquisicao.temperatura() < temp_baixa_) {
        alarmes.push_back(Alarme{
            "TEMP_BAIXA",
            "Temperatura do fermentador abaixo da faixa metabolica (risco de dormencia)",
            "aviso"});
    } else if (aquisicao.temperatura() > temp_alta_) {
        alarmes.push_back(Alarme{
            "TEMP_ALTA",
            "Temperatura critica no fermentador (risco de morte celular da levedura)",
            "critico"});
    }

    if (aquisicao_obsoleta) {
        alarmes.push_back(Alarme{
            "AQUISICAO_OBSOLETA",
            "Snapshot da planta possui timestamp com idade acima do limite operacional",
            "aviso"});
    }

    return alarmes;
}

}  // namespace fermentador
