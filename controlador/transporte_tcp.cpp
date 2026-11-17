#include "transporte_tcp.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace fermentador {

BufferEnquadramentoTcp::BufferEnquadramentoTcp(std::size_t tamanho_maximo)
    : buffer_(), tamanho_maximo_(tamanho_maximo) {
    if (tamanho_maximo_ < 64) {
        throw std::invalid_argument("tamanho_maximo do buffer TCP deve ser >= 64 bytes");
    }
}

std::vector<std::string> BufferEnquadramentoTcp::receberFragmento(const std::string& fragmento) {
    std::vector<std::string> mensagens;
    if (fragmento.empty()) {
        return mensagens;
    }

    buffer_.append(fragmento);

    std::size_t pos = 0;
    while ((pos = buffer_.find('\n')) != std::string::npos) {
        std::string linha = buffer_.substr(0, pos);
        buffer_.erase(0, pos + 1);
        if (!linha.empty() && linha.back() == '\r') {
            linha.pop_back();
        }
        if (!linha.empty()) {
            mensagens.push_back(std::move(linha));
        }
    }

    if (buffer_.size() > tamanho_maximo_) {
        buffer_.clear();
        throw std::overflow_error("quadro TCP excedeu tamanho maximo sem delimitador de linha");
    }

    return mensagens;
}

std::string BufferEnquadramentoTcp::enquadrarMensagem(const std::string& json_compacto) {
    std::string limpo;
    limpo.reserve(json_compacto.size() + 1);
    for (char c : json_compacto) {
        if (c != '\n' && c != '\r') {
            limpo.push_back(c);
        }
    }
    if (limpo.empty()) {
        throw std::invalid_argument("mensagem TCP nao pode ser vazia");
    }
    limpo.push_back('\n');
    return limpo;
}

void BufferEnquadramentoTcp::limparAoDesconectar() noexcept {
    buffer_.clear();
}

ClienteTcpPlanta::ClienteTcpPlanta(std::string host, int porta, int timeout_ms)
    : host_(std::move(host)),
      porta_(porta),
      sock_fd_(-1),
      timeout_ms_(timeout_ms),
      enquadrador_() {
    if (host_.empty()) {
        throw std::invalid_argument("host TCP nao pode ser vazio");
    }
    if (porta_ <= 0 || porta_ > 65535) {
        throw std::invalid_argument("porta TCP deve estar entre 1 e 65535");
    }
    if (timeout_ms_ <= 0) {
        throw std::invalid_argument("timeout_ms deve ser positivo");
    }
}

ClienteTcpPlanta::~ClienteTcpPlanta() {
    desconectar();
}

bool ClienteTcpPlanta::conectar() {
    desconectar();

    sock_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd_ < 0) {
        return false;
    }

    struct timeval tv;
    tv.tv_sec = timeout_ms_ / 1000;
    tv.tv_usec = (timeout_ms_ % 1000) * 1000;
    ::setsockopt(sock_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(sock_fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in endereco {};
    endereco.sin_family = AF_INET;
    endereco.sin_port = htons(static_cast<uint16_t>(porta_));
    if (::inet_pton(AF_INET, host_.c_str(), &endereco.sin_addr) <= 0) {
        desconectar();
        return false;
    }

    if (::connect(sock_fd_, reinterpret_cast<struct sockaddr*>(&endereco), sizeof(endereco)) < 0) {
        desconectar();
        return false;
    }

    return true;
}

void ClienteTcpPlanta::desconectar() noexcept {
    if (sock_fd_ >= 0) {
        ::close(sock_fd_);
        sock_fd_ = -1;
    }
    enquadrador_.limparAoDesconectar();
}

bool ClienteTcpPlanta::enviarQuadroJson(const std::string& json_texto) {
    if (!estaConectado()) {
        return false;
    }
    const std::string quadro = BufferEnquadramentoTcp::enquadrarMensagem(json_texto);
    std::size_t total_enviado = 0;
    while (total_enviado < quadro.size()) {
        const ssize_t n = ::send(
            sock_fd_,
            quadro.data() + total_enviado,
            quadro.size() - total_enviado,
            MSG_NOSIGNAL);
        if (n <= 0) {
            desconectar();
            return false;
        }
        total_enviado += static_cast<std::size_t>(n);
    }
    return true;
}

std::optional<std::string> ClienteTcpPlanta::receberQuadroJson() {
    if (!estaConectado()) {
        return std::nullopt;
    }

    // Verifica primeiro se já há um quadro extraído caso um fragmento anterior trouxesse múltiplos
    char buffer_leitura[1024];
    while (true) {
        const ssize_t n = ::recv(sock_fd_, buffer_leitura, sizeof(buffer_leitura), 0);
        if (n <= 0) {
            desconectar();
            return std::nullopt;
        }
        try {
            auto quadros = enquadrador_.receberFragmento(
                std::string(buffer_leitura, static_cast<std::size_t>(n)));
            if (!quadros.empty()) {
                return quadros.front();
            }
        } catch (...) {
            desconectar();
            return std::nullopt;
        }
    }
}

std::optional<std::string> ClienteTcpPlanta::requisitarSnapshotPlanta() {
    for (int tentativa = 0; tentativa < 2; ++tentativa) {
        if (!estaConectado() && !conectar()) {
            continue;
        }
        if (!enviarQuadroJson(R"({"acao":"OBTER_PLANTA"})")) {
            continue;
        }
        auto resposta = receberQuadroJson();
        if (resposta.has_value()) {
            return resposta;
        }
    }
    return std::nullopt;
}

bool ClienteTcpPlanta::enviarComandoPlanta(const std::string& json_comando) {
    for (int tentativa = 0; tentativa < 2; ++tentativa) {
        if (!estaConectado() && !conectar()) {
            continue;
        }
        if (!enviarQuadroJson(json_comando)) {
            continue;
        }
        auto ack = receberQuadroJson();
        if (ack.has_value()) {
            return true;
        }
    }
    return false;
}

}  // namespace fermentador
