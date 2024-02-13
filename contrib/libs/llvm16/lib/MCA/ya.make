# Generated by devtools/yamaker.

LIBRARY()

LICENSE(Apache-2.0 WITH LLVM-exception)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

PEERDIR(
    contrib/libs/llvm16
    contrib/libs/llvm16/lib/MC
    contrib/libs/llvm16/lib/Support
)

ADDINCL(
    contrib/libs/llvm16/lib/MCA
)

NO_COMPILER_WARNINGS()

NO_UTIL()

SRCS(
    CodeEmitter.cpp
    Context.cpp
    CustomBehaviour.cpp
    HWEventListener.cpp
    HardwareUnits/HardwareUnit.cpp
    HardwareUnits/LSUnit.cpp
    HardwareUnits/RegisterFile.cpp
    HardwareUnits/ResourceManager.cpp
    HardwareUnits/RetireControlUnit.cpp
    HardwareUnits/Scheduler.cpp
    IncrementalSourceMgr.cpp
    InstrBuilder.cpp
    Instruction.cpp
    Pipeline.cpp
    Stages/DispatchStage.cpp
    Stages/EntryStage.cpp
    Stages/ExecuteStage.cpp
    Stages/InOrderIssueStage.cpp
    Stages/InstructionTables.cpp
    Stages/MicroOpQueueStage.cpp
    Stages/RetireStage.cpp
    Stages/Stage.cpp
    Support.cpp
    View.cpp
)

END()