# 지원 엔진 버전

## 현황

| 엔진 | 상태 | 비고 |
|---|---|---|
| UE 5.7 | 🟡 개발 대상 | 이 버전의 소스를 읽고 작성. **빌드 검증 아직 안 됨** |
| UE 5.4 – 5.6 | ⚪ 미검증 | 구조상 동작할 가능성이 높으나 확인 필요 |
| UE 5.0 – 5.3 | ⚪ 미검증 | StateTree가 실험 단계라 API 차이가 클 수 있음 |
| UE 4.x | ❌ 미지원 | StateTree 자체가 없음 |

정직하게 적습니다: **실제로 돌려본 건 아직 없습니다.** 검증되면 이 표를 갱신하세요.

---

## 어떻게 버전 차이를 흡수하는가

### 1. 버전 분기는 한 파일에만

엔진 버전을 아는 파일은 **`StateTreeMCPCompat.h` / `.cpp` 둘뿐**입니다.
나머지 코드는 전부 `StateTreeMCPCompat::` 함수만 호출합니다. 시그니처는 고정이고
내부만 바뀝니다.

> 다른 파일에 `#if ENGINE_MINOR_VERSION`을 쓰고 싶어지면, 그건 Compat으로 옮기라는 신호입니다.

### 2. 버전 번호보다 "실제로 있는지" 확인

버전 번호는 거짓말을 합니다. 핫픽스로 필드가 생기기도 하고, 커스텀 엔진 브랜치도 있습니다.
그래서 두 층에서 실물을 확인합니다.

**빌드 시점** — `StateTreeMCP.Build.cs`가 엔진 폴더에 헤더가 실제로 있는지 봅니다:

```csharp
bool bHasStateTree = HasStateTreeHeader(Target, "StateTreeEditorData.h");
PublicDefinitions.Add($"STATETREEMCP_HAS_STATETREE={(bHasStateTree ? 1 : 0)}");
```

**실행 시점** — 리플렉션으로 프로퍼티 존재를 확인합니다:

```cpp
Out.bHasConsiderations = ClassHasProperty(UStateTreeState::StaticClass(), TEXT("Considerations"));
```

### 3. 없으면 곱게 죽기

StateTree가 없거나 꺼져 있어도 플러그인은 **정상 로드**됩니다. 대신 도구를 부르면
"왜 안 되는지"를 설명하는 에러를 돌려줍니다. `statetree_capabilities`는 어떤 상황에서도
답하므로, AI가 막힌 이유를 파악하고 다른 길을 찾을 수 있습니다.

### 4. StateTree 모듈은 지연 로드(delay-load)

`StateTreeModule` / `StateTreeEditorModule`은 엔진에 있을 때만 링크하고, Windows에서는
`PublicDelayLoadDLLs`로 등록합니다. 플러그인이 꺼져 있어도 **DLL 로드 실패로 에디터가
죽지 않습니다.**

---

## 새 엔진 버전 대응 절차

1. 그 버전으로 빌드 → 컴파일 에러 목록 확보
2. **`StateTreeMCPCompat.cpp`만** 수정 (필요하면 `.h`에 케이퍼빌리티 플래그 추가)
3. `Build.cs`의 헤더 탐지 경로가 그대로인지 확인
4. 에디터를 열고 `curl`로 `capabilities` → `describe_tree` 확인
5. 위 표 갱신

---

## 알려진 위험 지점

새 버전에서 깨질 가능성이 높은 순서:

| 위험 | 이유 | 대응 |
|---|---|---|
| `UStateTreeEditingSubsystem` 부재 | 구버전엔 없음 | `STATETREEMCP_HAS_EDITING_SUBSYSTEM`로 가드, 없으면 "에디터에서 직접 Compile 누르세요" 안내 |
| `UE_API` 매크로 | 최근 버전에서 `MinimalAPI` + `UE_API` 패턴으로 바뀜 | 헤더만 포함하면 되므로 대개 영향 없음 |
| `Considerations` / `TasksCompletion` | 비교적 최근 추가 | 런타임 리플렉션으로 탐지 |
| `HttpServer` API | 엔진 공용 모듈이라 비교적 안정적 | 인클루드 경로만 확인 |
| `UStateTreeState::Name` 등 필드명 | 변경 가능성 낮음 | 변경 시 Compat에서 흡수 |
