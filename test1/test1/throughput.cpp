#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/utils/safe_main.hpp>
#include <uhd/utils/thread.hpp>
#include <complex>
#include <chrono>
#include <atomic>
#include <thread>
#include <vector>
#include <queue>

namespace
{
    // 全局配置参数
    const double CENTER_FREQ = 1e9;       // 1GHz中心频率
    const double SAMPLE_RATE = 1e6;       // 1MS/s采样率
    const double TX_GAIN = 15.0;          // 发射增益
    const double RX_GAIN = 20.0;          // 接收增益
    const size_t SAMPS_PER_BUFFER = 4096; // 缓冲区大小
    const double RUN_TIME = 10.0;         // 运行时间(秒)
    const size_t NUM_TX_BUFFERS = 8;      // 发送缓冲区数量

    std::atomic<bool> stop_signal{ false };
    std::atomic<uint64_t> total_rx_samples{ 0 };
    std::atomic<uint64_t> total_tx_samples{ 0 };
}

// 吞吐量统计线程
void stats_thread()
{
    auto start_time = std::chrono::steady_clock::now();
    while (!stop_signal)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        auto duration = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start_time)
            .count();

        double tx_mbps = (total_tx_samples * sizeof(std::complex<float>) * 8) /
            (duration * 1e6);
        double rx_mbps = (total_rx_samples * sizeof(std::complex<float>) * 8) /
            (duration * 1e6);
        std::cout << "TX: " << tx_mbps << " Mbps | RX: " << rx_mbps << " Mbps"
            << " | Samples: " << total_rx_samples
            << " | Time: " << int(duration) << "s\n";
    }
}

int UHD_SAFE_MAIN(int argc, char* argv[])
{
    uhd::set_thread_priority_safe(1.0, true); // 设置实时优先级

    // 1. 创建并配置USRP
    auto usrp = uhd::usrp::multi_usrp::make(uhd::device_addr_t(""));
    usrp->set_tx_subdev_spec(uhd::usrp::subdev_spec_t("A:A"));
    usrp->set_rx_subdev_spec(uhd::usrp::subdev_spec_t("A:A"));

    // 配置射频参数
    usrp->set_tx_rate(SAMPLE_RATE);
    usrp->set_rx_rate(SAMPLE_RATE);
    usrp->set_tx_freq(CENTER_FREQ);
    usrp->set_rx_freq(CENTER_FREQ);
    usrp->set_tx_gain(TX_GAIN);
    usrp->set_rx_gain(RX_GAIN);
    usrp->set_clock_source("internal");
    usrp->set_time_source("internal");

    // 2. 生成测试信号（预生成多个缓冲区）
    std::vector<std::vector<std::complex<float>>> tx_buffs(NUM_TX_BUFFERS,
        std::vector<std::complex<float>>(SAMPS_PER_BUFFER));
    for (auto& buff : tx_buffs)
    {
        for (auto& samp : buff)
        {
            samp = std::complex<float>(
                (rand() % 2) * 2 - 1,
                (rand() % 2) * 2 - 1);
        }
    }

    // 3. 配置流参数
    uhd::stream_args_t tx_args("fc32");
    tx_args.args["spp"] = std::to_string(SAMPS_PER_BUFFER);
    tx_args.args["num_send_frames"] = "32"; // 增加发送帧缓冲
    auto tx_stream = usrp->get_tx_stream(tx_args);

    uhd::stream_args_t rx_args("fc32");
    rx_args.args["recv_buff_size"] = "16777216"; // 16MB接收缓冲
    auto rx_stream = usrp->get_rx_stream(rx_args);

    // 4. 启动统计线程
    std::thread stats(stats_thread);

    // 5. 异步发送线程
    std::atomic<bool> tx_done{ false };
    std::thread tx_thread([&]()
        {
            uhd::tx_metadata_t md;
            md.start_of_burst = true;
            md.end_of_burst = false;

            size_t buff_idx = 0;
            size_t num_sent = 0;
            const double timeout = 0.1; // 修改这里，设置超时时间为100ms

            while (!tx_done) {
                const auto& buff = tx_buffs[buff_idx];
                num_sent = tx_stream->send(buff.data(), buff.size(), md, timeout);

                if (num_sent < buff.size()) {
                    std::cerr << "TX Underflow! Sent " << num_sent << "/" << buff.size() << std::endl;
                }

                total_tx_samples += num_sent;
                buff_idx = (buff_idx + 1) % NUM_TX_BUFFERS;
                md.start_of_burst = false;
            }

            md.end_of_burst = true;
            tx_stream->send("", 0, md); });

    // 6. 接收循环
    uhd::stream_cmd_t rx_cmd(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
    rx_cmd.stream_now = true;
    rx_stream->issue_stream_cmd(rx_cmd);

    std::vector<std::complex<float>> rx_buff(SAMPS_PER_BUFFER * 4); // 更大的接收缓冲
    auto start_time = std::chrono::steady_clock::now();

    while (std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start_time)
        .count() < RUN_TIME)
    {

        uhd::rx_metadata_t rx_md;
        size_t num_rx = rx_stream->recv(rx_buff.data(), rx_buff.size(), rx_md); // 接收数据

        if (rx_md.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE)
        {
            std::cerr << "RX Error: " << rx_md.strerror() << std::endl;
            continue;
        }

        total_rx_samples += num_rx;
    }

    // 7. 停止设备
    stop_signal = true;
    tx_done = true;

    rx_stream->issue_stream_cmd(uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS);
    tx_thread.join();
    stats.join();

    std::cout << "\nTest completed. Final RX samples: " << total_rx_samples << std::endl;
    return 0;
}