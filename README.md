# RAG Pipeline Offloading via NVMe-oF for Edge LLM Inference

Edge 환경에서 대규모 RAG를 효율적으로 수행하기 위한 NVMe-oF 기반 RAG Pipeline Offloading 시스템입니다.
RAG 검색 과정에서 발생하는 원격 I/O와 네트워크 데이터 이동을 줄이고, Edge 환경에서도 대규모 Vector DB를 활용할 수 있도록 합니다.

---

## 1. 프로젝트 배경

최근 Large Language Model(LLM)은 다양한 분야에서 활용되고 있지만,
학습하지 않은 정보에 대해 잘못된 내용을 생성하는 환각(Hallucination) 문제와 최신 정보를 반영하기 어렵다는 한계가 있다.

Retrieval-Augmented Generation(RAG)은 외부 문서나 데이터베이스에서 관련 정보를 검색하여 LLM의 입력에 추가함으로써 이러한 문제를 완화한다.
하지만 RAG에서 사용하는 Vector DB의 규모가 커질수록 저장 공간과 데이터 접근에 대한 부담도 증가한다.

특히 자원이 제한된 Edge Device에서는 대규모 Vector DB를 로컬에 저장하기 어렵기 때문에 Vector DB를 별도의 Storage Server에 저장하는 구조가 필요하다.
이때 Vector DB는 Storage Server에 있지만 Similarity Search를 Edge Device에서 수행한다면, 검색 과정에서 원격 Vector Index와 데이터를 반복적으로 읽어야 한다.
이 과정에서 Edge Device와 Storage Server 사이에 반복적인 네트워크 I/O가 발생하며 데이터 이동량과 검색 지연이 증가할 수 있다.

### 1.1. 프로젝트 소개
본 프로젝트는 이러한 문제를 해결하기 위해
**RAG Pipeline을 데이터가 위치한 Storage Server에서 수행하는 Near-Data Processing 구조**를 구현한다.

기존에는 Edge Device가 원격 Storage Server의 Vector DB에 접근하여
RAG를 수행하지만, 제안 구조에서는 다음 과정을 Storage Server로
Offloading한다.

<img width="535" height="197" alt="image" src="https://github.com/user-attachments/assets/ea515e32-adbf-41e6-836b-42e4d687d832" />


Storage Server는 처리 결과로 생성된 **Token ID Sequence**를 Edge Device에
전달하고, Edge Device는 반환된 Token을 이용하여 LLM inference를 수행한다.

Edge Device와 Storage Server 간의 통신에는 **NVMe-oF/TCP**를 사용하며,
RAG Offloading 요청을 처리하기 위해 **Custom NVMe Command**를 사용한다.

---

## 2. 시스템 구성

전체 시스템은 크게 **Edge Device**와 **Storage Server**로 구성된다.
<img width="509" height="371" alt="image" src="https://github.com/user-attachments/assets/9a60c15a-4512-4024-ae91-9e6dc2f43c35" />


- **Edge Device**
  - 사용자 Query 입력
  - RAG Offloading 요청
  - 결과 Token 수신
  - LLM Inference 수행

- **Storage Server**
  - Vector DB 관리
  - Embedding
  - Similarity Search
  - Query Generation
  - Tokenization
  - 결과 Token 기록

두 장치는 **NVMe-oF/TCP**를 통해 연결된다.

### 2.1. 사용 기술

| 구분 | 기술 | 역할 |
|---|---|---|
| LLM Runtime | llama.cpp | Edge LLM 실행 및 Token 입력 처리 |
| LLM | Gemma 3 270M | 답변 생성 |
| 통신 | NVMe-oF/TCP | Edge와 Storage Server 연결 |
| Storage Framework | SPDK | NVMe-oF Target 및 Custom Command 처리 |
| Vector DB | PostgreSQL + pgvectorscale | Vector 저장 및 Similarity Search |
| Embedding | BGE-M3 | Query Embedding 생성 |
| Offloading Interface | Custom NVMe Command | RAG Offloading 요청 전달 |
| File Mapping | FIEMAP | 파일의 물리적 저장 영역 확인 |
| Result I/O | Direct I/O | Page Cache를 우회하여 결과 Token 읽기 |

---

## 3. 개발 결과

제안한 RAG Pipeline Offloading 구조의 효과를 확인하기 위해
기존 방식과 제안 방식의 데이터 이동량 및 RAG 단계별 처리 시간을 비교하였다.

실험 환경은 다음과 같다.

<img width="851" height="215" alt="image" src="https://github.com/user-attachments/assets/62884a87-1a7e-4b4e-aaae-44fbaae2b2d0" />


### 4.1. 데이터 이동량

Edge Device와 Storage Server 사이의 총 데이터 이동량을 측정한 결과 다음과 같다.
<img width="851" height="391" alt="image" src="https://github.com/user-attachments/assets/620a7bf1-e4aa-479d-b847-0ddc0229928a" />

| 방식 | 데이터 이동량 |
|---|---:|
| 기존 방식 | 121 MB |
| RAG Offloading | 33.1 MB |

제안 방식에서는 데이터 이동량이 **87.9 MB 감소**하였으며,
기존 방식 대비 약 **72.6% 감소**하였다.

### 4.2. RAG 단계별 Latency
<img width="851" height="374" alt="image" src="https://github.com/user-attachments/assets/b3ca525b-794c-46e6-820b-cddde01b47dd" />

| 단계 | 기존 방식 | Offloading | 변화 |
|---|---:|---:|---:|
| Embedding | 191.13 ms | 77.18 ms | 감소 |
| Similarity Search | 107.17 ms | 56.98 ms | 약 46.8% 감소 |
| Tokenization | 28.40 ms | 27.20 ms | 1.20 ms 감소 |

특히 Similarity Search를 Storage Server에서 수행함으로써
원격 Vector Index 접근 과정에서 발생하는 I/O를 줄일 수 있었다.

Similarity Search 내부의 I/O 시간은 다음과 같이 감소하였다.
<img width="851" height="365" alt="image" src="https://github.com/user-attachments/assets/54fc1e41-003c-4990-9c60-0ca1578ffdfc" />


```text
기존 방식      : 77.180 ms
RAG Offloading : 41.272 ms
```

약 **46.5% 감소**한 결과를 확인하였다.

이를 통해 데이터가 위치한 Storage Server에서 RAG Pipeline을 수행하는 구조가
Edge와 Storage Server 사이의 데이터 이동과 원격 데이터 접근 오버헤드를
감소시킬 수 있음을 확인하였다.

---

## 5. 설치 및 실행 방법
### 5.1. Repository Clone

```bash
git clone https://github.com/pnucse-capstone2026/capstone-2026-team-47.git
cd capstone-2026-team-47
```

---

### 5.2. Storage Server

SPDK 디렉터리로 이동한다.

```bash
cd src/storage-server/spdk-ndp
```

필요한 dependency를 설치한다.

```bash
sudo ./scripts/pkgdep.sh
```

SPDK를 build한다.

```bash
./configure
make -j$(nproc)
```

이후 NVMe SSD와 NVMe-oF Target을 설정하고
프로젝트에서 구현한 Storage Server 애플리케이션을 실행한다.

설정 시 필요한 주요 값은 다음과 같다.

---

### 5.3. Edge Device

llama.cpp 디렉터리로 이동한다.

```bash
cd src/edge/llama.cpp
```

Jetson 환경에서 llama.cpp를 build한다.

```bash
cmake -B build -DGGML_CUDA=ON
cmake --build build -j$(nproc)
```

Storage Server의 NVMe-oF Target에 연결한 후
연결된 NVMe Device와 Mount Path를 확인한다.

```bash
nvme list
```

프로젝트에서 사용하는 환경 변수를 설정한다.

```bash
export LLAMA_NVME_DEVICE=/dev/nvmeXn1
export LLAMA_MOUNT_PATH=/mnt/nvmeof
```

> 위 경로는 예시이며 실제 환경에 맞게 변경한다.

LLM을 실행한다.

```bash
./build/bin/llama-cli -m <MODEL_PATH>
```

RAG Pipeline Offloading을 사용하려면 CLI에서 다음 명령을 입력한다.

```text
/toggle-rag-offload
```

이후 Query를 입력하면 Storage Server에서 RAG Pipeline과
Tokenization을 수행한 뒤 반환된 Token ID를 이용하여
Edge Device에서 LLM inference가 수행된다.

## 6. 소개 자료

[![RAG Pipeline Offloading](https://img.youtube.com/vi/fTN8RPKtIdo/0.jpg)](https://www.youtube.com/watch?v=fTN8RPKtIdo)


## 7. 팀 구성

| 이름 | 주요 역할 |
|---|---|
| 양윤성 | Edge Device 및 llama.cpp 기반 LLM inference 구현 |
| 나예은 | Storage Server 및 SPDK 기반 RAG Pipeline 구현 |

## 8. 참고 문헌 및 출처
- **llama.cpp**
  - Original Project: https://github.com/ggml-org/llama.cpp
  - Modified Source: https://github.com/kmjstr35/llama.cpp

- **SPDK**
  - Original Project: https://github.com/spdk/spdk
  - Modified Source: https://github.com/kmjstr35/spdk-ndp

- **pgvectorscale**
  - https://github.com/timescale/pgvectorscale
