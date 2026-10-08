# Asahi Linux ─ Thunderbolt 與 Studio Display 支援

這是 [AsahiLinux/linux](https://github.com/AsahiLinux/linux) 的 fork，目標只有一個：

> **讓 Apple Studio Display 在 Apple Silicon 筆電上的 Linux 完整運作。**

包含畫面、喇叭、相機、USB hub、亮度調整，以及拔插之後能正常恢復。

上游 Asahi Linux 官方目前不支援 Thunderbolt，也因此不支援任何
Thunderbolt 螢幕。這個分支把缺的那幾塊補起來。

> ⚠️ **這份工作是人與 AI agent 協作完成的，不適合送往上游。**
> 詳見下方「[開發方式](#開發方式)」。

---

## 硬體範圍

| 項目 | 內容 |
|---|---|
| 實測機型 | MacBook Pro 14"（`j314s` / `t6000` / M1 Pro） |
| 螢幕 | Apple Studio Display（27" 5K，Thunderbolt） |
| 核心版本 | 7.1.13，基礎為上游 `fairydust` 分支 |
| 其他機型 | 裝置樹改動涵蓋 `t600x`，但**只在 j314s 上測過** |

只有一台機器、一個螢幕的實測。請當成實驗性質的東西看待。

---

## 做到了什麼

### Thunderbolt PCIe 隧道

Studio Display 不是普通的 DisplayPort 螢幕，它是一台 **Thunderbolt 裝置**。
它的喇叭、相機、USB hub 全部掛在一條 PCIe 隧道後面。Linux 缺的不是
DisplayPort 的驅動，而是 Apple 自家 ACIO 控制器的 NHI 驅動。

- 移植並延伸 [aurora-linux](https://github.com/brentkearney) 的 PCIe-C 實作
- 把 macOS 訓練隧道連結的暫存器順序逐步抄下來，而不是用猜的
- 每次插線重新套用 tunable
- 給隧道化的 root complex 正確的電源網域

**結果**：喇叭、相機、USB hub 都會動。

### 顯示輸出

- 透過 Thunderbolt 的 DP 隧道驅動 Studio Display，5120x2880 @ 60 Hz
- 拔掉 Type-C 線時正確釋放 CRTC
  （沒有這個的話，換一個孔插就會永久黑屏，直到重新登入）
- 不讓閒置、未連接的 CRTC 把整包 atomic commit 弄失敗
  （沒有這個的話，開機就黑屏）

### 亮度

Studio Display 的亮度走 **IOMFB**，不是 HID。
HID report 只是個鏡子，會回報數值但改不動它。

### 無線（AWDL / AirDrop）

brcmfmac 的 AWDL 支援，讓 AirDrop 能動。
包含 action frame 轉送、netdev 生命週期修正，以及韌體 RAM 快照的 vendor op。

---

## 還沒做到的

誠實列出來：

| 問題 | 狀況 |
|---|---|
| 睡眠耗電約 2.26 W | 睡一整晚會掉不少電。6 個顯示相關電源網域在睡眠時沒有斷電 |
| 睡醒後外接螢幕黑畫面 | 偶發。DP 鏈路在螢幕醒來前就被要求切模式。拔線重插可恢復 |
| 休眠（hibernate） | 完全不行，上游也不行 |
| Deep sleep / S2R | 不存在，只有 s2idle |

細節與進度都在 [Issues](https://github.com/AndyWeiBoan/linux/issues)。

---

## 分支

| 分支 | 說明 |
|---|---|
| `andywei/awdl` | **預設分支**，所有東西都在這。64 個自製 commit |
| `andywei/aurora-pciec` | PCIe-C 移植過程 |
| `andywei/tbt-*` | Thunderbolt 的各階段實驗 |
| `backup/*` | 歷史改寫前的備份，別動 |

---

## 編譯

跟一般的 Asahi 核心一樣：

```sh
make ARCH=arm64 defconfig       # 或用你現有的 .config
make ARCH=arm64 -j$(nproc) Image.gz modules
```

注意：`make` 不會自動重新產生 `Image.gz`，要明確指定。

### 相關的 module 參數

| 參數 | 預設 | 用途 |
|---|---|---|
| `appledrm.release_crtc_on_unplug` | `1` | 拔線時釋放 CRTC。關掉就會復現換孔黑屏 |
| `pcie_apple.tunnel_rearm` | — | 冷啟動前把活著的 PCIe-C 埠放回 reset |
| `pmgr_pwrstate.ignore_always_on` | — | 逗號分隔的網域名稱，用來做省電實驗 |

---

## 開發方式

**這個 fork 的程式碼是我與 AI agent（Claude）一起寫出來的。**

分工大致是：我負責實機測試、判讀現象、決定方向；agent 負責讀驅動原始碼、
比對 macOS 的行為、擬修法、寫 patch。所有改動都在這台機器上實際跑過，
而不是生成完就算數。相關的 commit 帶有 `Co-Authored-By` 標記。

### 這代表什麼

- **不會送往上游。** [Asahi Linux 的 Generative AI Policy](https://asahilinux.org/copyright/)
  禁止 AI 協助的貢獻。這個分支從一開始就不打算發 PR 給上游，
  存在的目的是讓我自己的機器能用，順便公開給同樣需要的人。
- **請自行判斷再使用。** 這些是核心層級的改動，碰到 PCIe、DMA 與電源網域。
  雖然實測過，但沒有經過上游的審查流程。

### 如果你想把這些東西送上游

那你得**重新獨立產出**，不能直接拿這裡的 patch。
不過裡面的發現（macOS 的隧道連結訓練順序、Studio Display 的亮度走 IOMFB
而非 HID、顯示 CRTC 的釋放時機等）是硬體事實，那部分可以自由參考。

---

## 來源與致謝

這份工作站在很多人的肩膀上：

- **[Asahi Linux](https://asahilinux.org/)** — 整個 Apple Silicon 的 Linux 移植
- **Brent Kearney（aurora-linux）** — PCIe-C 隧道的原始實作
- **Sven Peter**、**Janne Grunau** — Apple Silicon 的 PCIe、DART、DCP、裝置樹

他們的 commit 都保留原作者署名。

---

## 授權

GPL-2.0，跟 Linux 核心一樣。

核心本身的說明文件請看 `Documentation/admin-guide/README.rst`，
編譯與安裝步驟在 `Documentation/process/changes.rst`。
