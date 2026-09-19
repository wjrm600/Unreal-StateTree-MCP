# Unreal StateTree MCP

언리얼 엔진의 **StateTree 에셋을 AI 어시스턴트가 직접 읽고 편집**할 수 있게 해주는 MCP 서버입니다.

> **현재 상태: UE 5.7.4에서 동작 검증 완료.** 빌드·읽기·쓰기·컴파일·저장 전 구간을 실제
> 프로젝트에서 확인했습니다. 다른 엔진 버전은 아직 미검증입니다 —
> [docs/VERSION_SUPPORT.md](docs/VERSION_SUPPORT.md) 참고.

---

## 왜 C++ 플러그인인가

언리얼 자동화는 보통 Python으로 합니다. **StateTree는 안 됩니다.**

에디터에서 상태 계층을 들고 있는 두 배열이 이렇게 선언되어 있기 때문입니다:

```cpp
// StateTreeEditorData.h — 루트 상태들
UPROPERTY(Instanced)
TArray<TObjectPtr<UStateTreeState>> SubTrees;

// StateTreeState.h — 자식 상태들
UPROPERTY(Instanced)
TArray<TObjectPtr<UStateTreeState>> Children;
```

`EditAnywhere`도 `BlueprintReadWrite`도 없습니다. 언리얼의 프로퍼티 접근 관문
(`PropertyAccessUtil::CanGetPropertyValue`)은 `CPF_Edit`, `CPF_BlueprintVisible`,
`CPF_BlueprintAssignable` 중 **하나는 있어야** 통과시킵니다. 셋 다 없으므로
Python에서는 `get_editor_property("children")`조차 `PermissionDenied`로 막힙니다.

**즉, Python은 트리의 모양 자체를 볼 수 없습니다.** 반면 C++에서는 둘 다 `public`
멤버라 그냥 읽힙니다. 이 플러그인이 하는 일의 절반은 **C++에서만 보이는 구조를
JSON으로 바깥에 내보내는 것**입니다.

컴파일도 마찬가지입니다. `UStateTreeEditingSubsystem::CompileStateTree()`는
`UFUNCTION`이 아니라 순수 C++ static 함수라 스크립트에서 호출할 수 없습니다.

---

## 구조

```
Claude  ──stdio──▶  MCP 서버 (Python)  ──HTTP──▶  언리얼 에디터 플러그인 (C++)
                    mcp-server/                   UnrealPlugin/StateTreeMCP/
                                                  127.0.0.1:8092/rpc
```

엔드포인트는 `POST /rpc` **하나**입니다. 요청은 `{"action": "...", "params": {...}}`,
응답은 `{"ok": true, "result": {...}}` 또는 `{"ok": false, "error": "..."}`.
도구를 추가해도 라우팅은 그대로고, `curl` 한 줄로 손 테스트가 됩니다.

```
UnrealPlugin/StateTreeMCP/Source/StateTreeMCP/
├─ Public/
│  ├─ StateTreeMCPCompat.h      ← 엔진 버전 차이를 아는 유일한 파일
│  ├─ StateTreeMCPSubsystem.h   ← HTTP 브리지
│  └─ StateTreeMCPModule.h
└─ Private/
   ├─ StateTreeMCPCompat.cpp
   ├─ StateTreeMCPSubsystem.cpp ← 도구 핸들러
   └─ StateTreeMCPModule.cpp
```

---

## 도구

| 도구 | 하는 일 |
|---|---|
| `statetree_capabilities` | 연결된 엔진 버전과 지원 기능 보고 |
| `statetree_describe` | 에셋의 전체 상태 목록 (id / 이름 / 깊이 / 부모) |
| `statetree_add_state` | 상태 추가 |
| `statetree_compile` | 검증 + 컴파일 (기본으로 저장까지) |
| `statetree_save` | 디스크에 저장 |

**도구 이름은 엔진 버전과 무관하게 고정입니다.** 버전 차이는 안쪽 구현이 흡수합니다.

---

## 설치

### 1. 플러그인

`UnrealPlugin/StateTreeMCP/` 폴더를 프로젝트의 `Plugins/`로 복사합니다.

```powershell
Copy-Item -Recurse "<이 저장소>\UnrealPlugin\StateTreeMCP" "<프로젝트>\Plugins\StateTreeMCP"
```

`.uproject`에 추가:

```json
{ "Name": "StateTree",    "Enabled": true },
{ "Name": "StateTreeMCP", "Enabled": true }
```

`.uproject` 우클릭 → **Generate Visual Studio project files** → 빌드.

### 2. MCP 서버

의존성은 `mcp` 하나뿐입니다. 다른 MCP 서버를 이미 쓰고 있다면 대개 깔려 있습니다.

```bash
python -c "import mcp" || pip install mcp
```

**패키지 설치(`pip install -e .`)는 권장하지 않습니다.** 아래처럼 `PYTHONPATH`로
저장소를 직접 가리키면, 코드를 고쳤을 때 설치본과 저장소가 어긋날 일이 없습니다.

### 3. Claude에 등록

```json
"unreal-statetree": {
  "command": "python",
  "args": ["-m", "statetree_mcp"],
  "env": {
    "PYTHONPATH": "<이 저장소>\mcp-server",
    "STATETREE_MCP_PORT": "8092"
  }
}
```

Windows 경로는 JSON 안에서 **역슬래시를 두 번** 써야 합니다
(`E:\Unreal Project\...`). 한 번만 쓰면 파싱 오류가 납니다.

**어느 파일에 넣느냐가 중요합니다** — 쓰는 클라이언트마다 읽는 곳이 다릅니다.

| 클라이언트 | 설정 파일 |
|---|---|
| Claude 데스크톱 앱 (Windows) | `%LOCALAPPDATA%\Packages\Claude_*\LocalCache\Roaming\Claude\claude_desktop_config.json` |
| Claude 데스크톱 앱 (macOS) | `~/Library/Application Support/Claude/claude_desktop_config.json` |
| Claude Code (프로젝트 단위) | 프로젝트 루트의 `.mcp.json` |

데스크톱 앱 안의 Code 탭도 **데스크톱 설정 파일**을 읽습니다. 둘 다 쓴다면 양쪽에
같은 항목을 넣으면 됩니다. **설정을 고친 뒤에는 앱을 완전히 재시작해야 반영됩니다.**

> 이 서버는 **언리얼 에디터가 켜져 있어야** 동작합니다. 꺼져 있으면 도구가
> "Could not reach the Unreal Editor on port 8092..." 라고 답합니다.

---

## 빌드하기

`.uproject` 우클릭 → **Generate Visual Studio project files** → 빌드.
새 모듈이라 **핫 리로드로는 안 되고 풀 리빌드가 필요합니다.**

에디터를 열고 출력 로그에서 확인하세요:

```
LogStateTreeMCP: StateTree MCP starting on UE 5.7 (StateTree=yes, compile=yes, ...)
LogStateTreeMCP: StateTree MCP bridge listening on http://127.0.0.1:8092/rpc
```

브리지 단독 테스트 (MCP 서버 없이):

```bash
curl -X POST http://127.0.0.1:8092/rpc -H "Content-Type: application/json" -d "{\"action\":\"capabilities\"}"
```

### 다른 엔진 버전으로 옮길 때 참고

5.7 첫 빌드에서 걸렸던 것들입니다. 같은 자리에서 막힐 가능성이 높습니다.

| 증상 | 원인 | 해결 |
|---|---|---|
| `C2371` / `C2064` | `FHttpResultCallback`은 클래스가 아니라 `TFunction`의 typedef라 전방 선언 불가 | `HttpResultCallback.h` 직접 include |
| `C2679` / `C2664` | `BindRoute`가 돌려주는 건 `FDelegateHandle`이 아니라 `FHttpRouteHandle` | 멤버 타입 교체 |
| `LNK2019` 소멸자 미해결 | `FStateTreeCompilerLog` → `FStateTreeBindableStructDesc` → `FPropertyBindingBindableStructDescriptor`의 vtable이 다른 플러그인 소유 | `PropertyBindingUtils` 모듈 링크 (**지연 로드 금지** — vtable은 데이터 심볼) |
| 짧은 요청만 "invalid JSON" | `Request.Body`는 종료 문자가 없는 바이트 배열 | 복사 후 `0` 추가하고 변환 |

---

## 포트 충돌

기본 포트는 **8092**입니다. 다른 언리얼 MCP 브리지가 흔히 8091을 쓰기 때문에 피했습니다.
바꾸려면 에디터 실행 인자에 `-StateTreeMCPPort=9000`을 주고, MCP 서버에는
`STATETREE_MCP_PORT=9000`을 맞춰주세요.

---

## 지원 엔진 버전

[docs/VERSION_SUPPORT.md](docs/VERSION_SUPPORT.md) 참고. 요약: **UE 5.7에서 개발했고
다른 버전은 아직 미검증**입니다. 새 버전 대응은 `StateTreeMCPCompat.h/.cpp`만 고치면
되도록 설계했습니다.

---

## 라이선스

MIT — [LICENSE](LICENSE)
