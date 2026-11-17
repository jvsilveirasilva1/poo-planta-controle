#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace fermentador {

/**
 * Gerencia o enquadramento (framing) de mensagens JSON sobre stream TCP (NDJSON: 1 JSON por linha '\n').
 * Resolve os dois problemas clássicos de transporte em SOCK_STREAM:
 * 1. Fragmentação: um JSON dividido em vários pacotes/chamadas recv() é reconstruído até chegar '\n'.
 * 2. Coalescência: múltiplos JSONs recebidos juntos no mesmo recv() são separados individualmente.
 * 3. Desconexão/Recuperação: limpa bytes parciais não terminados em '\n' ao reconectar.
 */
class BufferEnquadramentoTcp {
    std::string buffer_;
    std::size_t tamanho_maximo_;

public:
    explicit BufferEnquadramentoTcp(std::size_t tamanho_maximo = 65536);

    /**
     * Acrescenta um fragmento de bytes recebidos do socket e extrai todas as mensagens
     * completas (terminadas por '\n') disponíveis até o momento.
     */
    std::vector<std::string> receberFragmento(const std::string& fragmento);

    /**
     * Formata um documento JSON como quadro NDJSON (remove quebras internas e garante '\n' final).
     */
    static std::string enquadrarMensagem(const std::string& json_compacto);

    /**
     * Descarta fragmentos incompletos pendentes quando ocorre queda de conexão TCP,
     * evitando contaminar o primeiro quadro após a reconexão.
     */
    void limparAoDesconectar() noexcept;

    std::size_t bytesPendentes() const noexcept { return buffer_.size(); }
    std::size_t tamanhoMaximo() const noexcept { return tamanho_maximo_; }
};

/**
 * Cliente TCP POSIX para comunicação entre o Controlador C++ e o Servidor TCP da Planta.
 * Suporta envio/recebimento com reconstrução de fragmentos, timeout e reconexão automática.
 */
class ClienteTcpPlanta {
    std::string host_;
    int porta_;
    int sock_fd_;
    int timeout_ms_;
    BufferEnquadramentoTcp enquadrador_;

public:
    ClienteTcpPlanta(std::string host, int porta, int timeout_ms = 3000);
    ~ClienteTcpPlanta();

    ClienteTcpPlanta(const ClienteTcpPlanta&) = delete;
    ClienteTcpPlanta& operator=(const ClienteTcpPlanta&) = delete;

    bool conectar();
    void desconectar() noexcept;
    bool estaConectado() const noexcept { return sock_fd_ >= 0; }

    /**
     * Envia um quadro JSON completo garantido (lida com escrita parcial de send()).
     */
    bool enviarQuadroJson(const std::string& json_texto);

    /**
     * Lê do socket TCP (acumulando fragmentos em BufferEnquadramentoTcp) até formar
     * um quadro JSON completo terminado por '\n', ou retorna nullopt em desconexão/timeout.
     */
    std::optional<std::string> receberQuadroJson();

    /**
     * Solicita o snapshot atual da planta ("OBTER_PLANTA"), mantendo reconexão automática
     * caso o socket tenha caído.
     */
    std::optional<std::string> requisitarSnapshotPlanta();

    /**
     * Envia comando_planta.json via TCP e aguarda confirmação de recebimento ("ACK_TCP")
     * ou fecha a conexão para recuperação no próximo ciclo.
     */
    bool enviarComandoPlanta(const std::string& json_comando);

    const std::string& host() const noexcept { return host_; }
    int porta() const noexcept { return porta_; }
};

}  // namespace fermentador
