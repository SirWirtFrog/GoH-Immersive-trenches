EXTERN GemConstructionOriginal:QWORD
EXTERN LogGemConstruction:PROC

.code

TRACE_INSTALL MACRO entryName, entryIndex
PUBLIC entryName
entryName PROC FRAME
    sub rsp, 98h
    .allocstack 98h
    .endprolog

    ; Keep the Win64 argument registers and leave the caller's stack arguments in place.
    mov [rsp + 30h], rcx
    mov [rsp + 38h], rdx
    mov [rsp + 40h], r8
    mov [rsp + 48h], r9
    movaps [rsp + 50h], xmm0
    movaps [rsp + 60h], xmm1
    movaps [rsp + 70h], xmm2
    movaps [rsp + 80h], xmm3

    mov [rsp + 20h], r9
    IF entryIndex EQ 0
        ; install reads the target and mode from its fifth and sixth arguments.
        lea rax, [rsp + 0c0h]
        mov [rsp + 28h], rax
    ELSE
        mov qword ptr [rsp + 28h], 0
    ENDIF
    mov r9, r8
    mov r8, rdx
    mov rdx, rcx
    mov ecx, entryIndex
    call LogGemConstruction

    mov rcx, [rsp + 30h]
    mov rdx, [rsp + 38h]
    mov r8, [rsp + 40h]
    mov r9, [rsp + 48h]
    movaps xmm0, [rsp + 50h]
    movaps xmm1, [rsp + 60h]
    movaps xmm2, [rsp + 70h]
    movaps xmm3, [rsp + 80h]
    add rsp, 98h
    jmp qword ptr [GemConstructionOriginal + 8 * entryIndex]
entryName ENDP
ENDM

TRACE_INSTALL GemTraceInstall, 0
TRACE_INSTALL GemTraceInstallBegin, 1
TRACE_INSTALL GemTraceInstallEnd, 2
TRACE_INSTALL GemTraceExitSpawn, 4

END
