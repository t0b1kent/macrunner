/* Проверка карты host offset -> guest RIP на СЛИЯНИЯХ.
 *
 * Зачем. Ранг 1 (снять снимок контекста с каждой диспетчеризации, восстанавливать позицию по
 * карте) уже включается гейтом MACRUNNER_HB_NO_SNAPSHOT. Замер показал, что копия действительно
 * уходит: гард 87-133 -> 17 выборок, memmove 63-93 -> 2. Но правильность замены НЕ подтверждена:
 * трасса ripmap-progress за прогон не напечатала ни строки, потому что путь восстановления
 * холодный (снимок восстанавливался 0 раз на 741 M диспетчеризаций). Механизм, который никогда не
 * исполняется в обычном прогоне, обычным прогоном и не проверить.
 *
 * Что именно проверяется. Дефект, найденный 01.08, был в СООТВЕТСТВИИ ИНДЕКСОВ: запись в карту
 * шла по номеру итерации, а слияния двигают индекс инструкции на 2-4. В любом блоке со слиянием
 * резолвер отдавал чужой гостевой адрес — молча и в самом частом случае, потому что CMP/Jcc это
 * наш основной путь. Исправление добавило явное поле host_instr[].
 *
 * Проверять это отказом в живой игре не нужно и не надёжно. Достаточно собрать блок, в котором
 * слияние гарантированно срабатывает И ПОСЛЕ НЕГО ЕСТЬ ПРОДОЛЖЕНИЕ, и убедиться в двух вещах:
 *   1. host_instr[] строго возрастает — иначе двоичный поиск по host_off[] бессмыслен;
 *   2. host_instr[n] > n хотя бы в одной записи — это и есть доказательство, что слияние
 *      произошло И что индекс записан отдельно от номера записи. Если бы всюду было
 *      host_instr[n] == n, тест прошёл бы и на старом, сломанном коде.
 *
 * Второе условие — то, ради чего тест написан: оно отличает исправленный код от сломанного,
 * а не просто проверяет, что ничего не падает.
 *
 * ПРОВЕРЕНО НА СЕБЕ. Первая версия брала CMP+Jcc и провалилась — не из-за кода, а из-за образца:
 * Jcc завершает блок, после слияния инструкций нет, расхождение возникнуть не может. Такой тест
 * прошёл бы и на сломанном коде. На SETcc, за которым идут ещё две инструкции, запись 2 указывает
 * на инструкцию 3 — расхождение налицо, и старый код на нём бы упал. */
#include "hb_context.h"
#include "hb_ir.h"
#include "hb_runtime.h"
#include "hb_codegen.h"
#include <stdio.h>
#include <string.h>

static int fail;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  ПРОВАЛ: %s (%s:%d)\n", msg, __FILE__, __LINE__); fail = 1; } \
    else printf("  ок: %s\n", msg); } while (0)

int main(void) {
    hb_ir_func_t* func = hb_ir_func_create(0x1000, 0);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x1000);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;

    /* ОБРАЗЕЦ ВЫБРАН СПЕЦИАЛЬНО. Первая версия теста строилась на CMP+Jcc — единственном слиянии,
     * которое НЕ может дать расхождение индексов: Jcc завершает блок, эмиссия обрывается на первой
     * передаче управления, и после слияния инструкций уже нет. Тот тест прошёл бы и на старом,
     * сломанном коде — то есть был неспособен увидеть искомое.
     *
     * Из четырёх мест, где цикл эмиссии перескакивает инструкции, ТРИ не завершают блок:
     * emit_stack_spill_push_sub_prologue (i += 3), emit_xfg_dispatch_call_pair (i += 2) и
     * emit_scalar_flags_setcc_sequence (i += 2). Берём последнее: арифметика, задающая флаги,
     * затем SETcc E/NE в 8-битный регистр — и ПОСЛЕ него ещё инструкции, чтобы счётчик записей
     * отстал от индекса и расхождение стало видимым. */
    hb_ir_builder_t* b = hb_ir_builder_create(func);
    hb_ir_builder_set_block(b, blk);
    hb_ir_emit_mov(b, hb_ir_reg(HB_REG_RAX, HB_SIZE_64), hb_ir_imm(5, HB_SIZE_64));
    hb_ir_emit_binop(b, HB_IR_AND, hb_ir_reg(HB_REG_RAX, HB_SIZE_64),
                     hb_ir_reg(HB_REG_RAX, HB_SIZE_64), hb_ir_imm(3, HB_SIZE_64));
    hb_ir_emit_setcc(b, HB_CC_E, hb_ir_reg(HB_REG_RCX, HB_SIZE_8));   /* сливается с AND выше */
    hb_ir_emit_mov(b, hb_ir_reg(HB_REG_RDX, HB_SIZE_64), hb_ir_imm(9, HB_SIZE_64));
    hb_ir_emit_mov(b, hb_ir_reg(HB_REG_RBX, HB_SIZE_64), hb_ir_imm(11, HB_SIZE_64));
    hb_ir_emit_ret(b);
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
    hb_codegen_buffer_t* buf = hb_codegen_buffer_create(65536);
    hb_result_t r = hb_arm64_codegen_block(cg, blk, buf);

    printf("codegen=%d  инструкций в блоке=%zu  записей в карте=%zu\n",
           (int)r, blk->instr_count, (size_t)buf->host_off_count);
    CHECK(r == HB_OK, "кодогенерация прошла");
    CHECK(buf->host_off_count > 0, "карта не пуста");

    /* 1. строгое возрастание — предпосылка двоичного поиска */
    int mono = 1, off_mono = 1;
    for (size_t n = 1; n < buf->host_off_count; n++) {
        if (buf->host_instr[n] <= buf->host_instr[n - 1]) mono = 0;
        if (buf->host_off[n] < buf->host_off[n - 1]) off_mono = 0;
    }
    CHECK(mono, "host_instr[] строго возрастает");
    CHECK(off_mono, "host_off[] не убывает");

    /* 2. индексы РАСХОДЯТСЯ с номерами записей — доказательство слияния и раздельной записи */
    int diverged = 0;
    for (size_t n = 0; n < buf->host_off_count; n++)
        if (buf->host_instr[n] != (uint16_t)n) diverged = 1;
    CHECK(diverged,
          "host_instr[n] != n хотя бы раз (слияние сработало И индекс записан отдельно)");

    /* 3. индекс не выходит за пределы блока — иначе резолвер прочитает мусор */
    int in_range = 1;
    for (size_t n = 0; n < buf->host_off_count; n++)
        if (buf->host_instr[n] >= blk->instr_count) in_range = 0;
    CHECK(in_range, "все host_instr[] внутри блока");

    for (size_t n = 0; n < buf->host_off_count && n < 12; n++)
        printf("    запись %2zu: host_off=%5u  host_instr=%2u  guest=0x%llx\n",
               n, buf->host_off[n], buf->host_instr[n],
               (unsigned long long)blk->instrs[buf->host_instr[n]].guest_addr);

    hb_codegen_buffer_destroy(buf);
    hb_arm64_codegen_destroy(cg);
    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    printf(fail ? "\nИТОГ: ПРОВАЛ\n" : "\nИТОГ: всё сошлось\n");
    return fail;
}
