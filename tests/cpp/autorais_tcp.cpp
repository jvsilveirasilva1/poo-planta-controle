#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "aquisicao.hpp"
#include "controlador.hpp"
#include "telemetria.hpp"
#include "transporte_tcp.hpp"
#include "vendor/nlohmann/json.hpp"

namespace {

void verificar(bool condicao, const std::string& mensagem) {
    if (!condicao) {
        throw std::runtime_error(mensagem);
    }
}

void testarFragmentacaoCoalescenciaERecuperacaoBuffer() {
    fermentador::BufferEnquadramentoTcp buffer(128);

    const std::string json_msg = R"({"versao":1,"sessao":"s-tcp","ciclo":1})";
    const std::string quadro = fermentador::BufferEnquadramentoTcp::enquadrarMensagem(json_msg);

    // 1. Entrega fragmentada de 3 em 3 bytes
    std::vector<std::string> extraidas;
    for (std::size_t i = 0; i < quadro.size(); i += 3) {
        auto lote = buffer.receberFragmento(quadro.substr(i, 3));
        extraidas.insert(extraidas.end(), lote.begin(), lote.end());
    }
    verificar(extraidas.size() == 1, "fragmentos de 3 bytes devem reconstruir exatamente 1 mensagem");
    verificar(extraidas[0] == json_msg, "mensagem reconstruida difere da original");
    verificar(buffer.bytesPendentes() == 0, "buffer deve ficar vazio apos delimitador");

    // 2. Coalescência: 2 quadros concatenados no mesmo recv()
    const std::string msg1 = R"({"id":1})";
    const std::string msg2 = R"({"id":2})";
    auto coalescidas = buffer.receberFragmento(msg1 + "\n" + msg2 + "\r\n");
    verificar(coalescidas.size() == 2, "dois quadros no mesmo pacote devem gerar 2 mensagens");
    verificar(coalescidas[0] == msg1 && coalescidas[1] == msg2, "ordem ou conteudo incorreto na coalescencia");

    // 3. Desconexão no meio de um quadro parcial limpa resíduos sem contaminar a reconexão
    auto parcial = buffer.receberFragmento(R"({"incompleto":12)");
    verificar(parcial.empty() && buffer.bytesPendentes() > 0, "quadro sem '\\n' deve aguardar no buffer");
    buffer.limparAoDesconectar();
    verificar(buffer.bytesPendentes() == 0, "limparAoDesconectar deve zerar bytes pendentes");

    auto pos_reconexao = buffer.receberFragmento(R"({"integro":true})" "\n");
    verificar(pos_reconexao.size() == 1 && pos_reconexao[0] == R"({"integro":true})",
              "primeiro quadro apos reconexao nao pode ser contaminado pelo fragmento anterior");

    // 4. Estouro de buffer sem '\n' lança std::overflow_error e limpa o buffer
    bool lancou_overflow = false;
    try {
        buffer.receberFragmento(std::string(200, 'X'));
    } catch (const std::overflow_error&) {
        lancou_overflow = true;
    }
    verificar(lancou_overflow && buffer.bytesPendentes() == 0,
              "quadro gigante sem delimitador deve lancar overflow_error e limpar o buffer");
}

void testarControladorTcpComFragmentacaoQuedaEReconexao() {
    int srv_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    verificar(srv_fd >= 0, "falha ao criar socket servidor de teste");

    int opt = 1;
    ::setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;  // Porta efêmera escolhida pelo kernel
    verificar(::bind(srv_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0,
              "falha no bind loopback");
    verificar(::listen(srv_fd, 4) == 0, "falha no listen loopback");

    socklen_t len = sizeof(addr);
    verificar(::getsockname(srv_fd, reinterpret_cast<struct sockaddr*>(&addr), &len) == 0,
              "falha no getsockname");
    const int porta = ntohs(addr.sin_port);

    const std::string snapshot_c1 = R"({"versao":1,"sessao":"tcp-s1","ciclo":1,"timestamp":"2026-10-21T18:00:00Z","leituras":[{"tag":"LT-01","tipo":"nivel","valor":50.0,"unidade":"%","status":"operando"},{"tag":"TT-01","tipo":"temperatura","valor":25.0,"unidade":"C","status":"operando"}],"atuadores":{"bomba":0.0,"aquecedor":0.0},"ultimo_comando":0,"erro_comando":null,"motivo_seguro":"sem_arquivo"})";
    const std::string snapshot_c2 = R"({"versao":1,"sessao":"tcp-s1","ciclo":2,"timestamp":"2026-10-21T18:00:01Z","leituras":[{"tag":"LT-01","tipo":"nivel","valor":52.0,"unidade":"%","status":"operando"},{"tag":"TT-01","tipo":"temperatura","valor":26.5,"unidade":"C","status":"operando"}],"atuadores":{"bomba":100.0,"aquecedor":100.0},"ultimo_comando":1,"erro_comando":null,"motivo_seguro":null})";

    std::string comando_recebido_c1;
    std::string comando_recebido_c2;

    std::thread servidor([&]() {
        char buf[512];
        // Conexão 1: Envia o snapshot_c1 FRAGMENTADO em 3 partes e recebe o comando do ciclo 1
        int c1 = ::accept(srv_fd, nullptr, nullptr);
        if (c1 >= 0) {
            (void)::recv(c1, buf, sizeof(buf), 0);  // Lê {"acao":"OBTER_PLANTA"}
            std::string quadro1 = snapshot_c1 + "\n";
            std::size_t terco = quadro1.size() / 3;
            (void)::send(c1, quadro1.data(), terco, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            (void)::send(c1, quadro1.data() + terco, terco, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            (void)::send(c1, quadro1.data() + 2 * terco, quadro1.size() - 2 * terco, 0);

            // Lê comando_planta.json do ciclo 1 e responde ACK
            ssize_t n = ::recv(c1, buf, sizeof(buf), 0);
            if (n > 0) {
                comando_recebido_c1.assign(buf, static_cast<std::size_t>(n));
                const std::string ack = R"({"status":"ok"})" "\n";
                (void)::send(c1, ack.data(), ack.size(), 0);
            }
            // Simula queda abrupta da conexão após o ciclo 1!
            ::close(c1);
        }

        // Conexão 2 (Reconexão automática do cliente no ciclo 2 após queda da conexão 1):
        int c2 = ::accept(srv_fd, nullptr, nullptr);
        if (c2 >= 0) {
            (void)::recv(c2, buf, sizeof(buf), 0);  // Lê {"acao":"OBTER_PLANTA"}
            std::string quadro2 = snapshot_c2 + "\n";
            (void)::send(c2, quadro2.data(), quadro2.size(), 0);

            ssize_t n = ::recv(c2, buf, sizeof(buf), 0);
            if (n > 0) {
                comando_recebido_c2.assign(buf, static_cast<std::size_t>(n));
                const std::string ack = R"({"status":"ok"})" "\n";
                (void)::send(c2, ack.data(), ack.size(), 0);
            }
            ::close(c2);
        }
        ::close(srv_fd);
    });

    const auto dir_temp = std::filesystem::temp_directory_path() / "teste_tcp_bonus_poo";
    std::filesystem::remove_all(dir_temp);
    std::filesystem::create_directories(dir_temp);

    fermentador::ClienteTcpPlanta cliente("127.0.0.1", porta, 2000);
    fermentador::Controlador ctrl;
    const std::time_t t0 =
        fermentador::ValidadorAquisicao::converterTimestampUtc("2026-10-21T18:00:00Z");

    // Executa Ciclo 1 sobre TCP (recebendo snapshot fragmentado em 3 pacotes)
    verificar(ctrl.processarCicloTcp(cliente, dir_temp, t0) == 0,
              "processarCicloTcp deve ter sucesso no ciclo 1 com pacotes fragmentados");

    // Executa Ciclo 2 sobre TCP (o servidor fechou o socket anterior; o cliente deve reconectar e preservar a sessão!)
    verificar(ctrl.processarCicloTcp(cliente, dir_temp, t0 + 1) == 0,
              "processarCicloTcp deve reconectar automaticamente apos queda e processar ciclo 2");

    servidor.join();

    const auto doc_cmd1 = nlohmann::json::parse(comando_recebido_c1);
    const auto doc_cmd2 = nlohmann::json::parse(comando_recebido_c2);
    verificar(doc_cmd1["sessao"] == "tcp-s1" && doc_cmd1["id"] == 1,
              "comando TCP do ciclo 1 deve ter id=1");
    verificar(doc_cmd2["sessao"] == "tcp-s1" && doc_cmd2["id"] == 2,
              "comando TCP do ciclo 2 apos reconexao deve preservar sessao e avancar para id=2");
    verificar(ctrl.gestorComandos().ultimoIdConfirmado() == 1,
              "controlador deve ter confirmado id=1 ao receber ciclo 2 via TCP");

    std::filesystem::remove_all(dir_temp);
}

}  // namespace

int main() {
    try {
        testarFragmentacaoCoalescenciaERecuperacaoBuffer();
        testarControladorTcpComFragmentacaoQuedaEReconexao();
        std::cout << "OK: Bonus TCP (enquadramento, fragmentacao, desconexao e reconexao) verificado\n";
        return 0;
    } catch (const std::exception& erro) {
        std::cerr << "FALHOU: " << erro.what() << '\n';
        return 1;
    }
}
