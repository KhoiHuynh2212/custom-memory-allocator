# Context — Custom Allocator (dlmalloc-style) Debug Session

File này tóm tắt các bug đã tìm ra, đã sửa, và các đề xuất **chưa áp dụng** trong phiên debug hiện tại.
Repo tham chiếu: `my-malloc.c`, `internal.h`, `list.h`, `test/test-edge-cases.c`.

---

## 1. Bug gốc (ĐÃ SỬA): NULL write trong `list_unlink`

**Triệu chứng:** ASan báo SEGV trong `list_unlink` (`src/list.h:265`), stack: `list_unlink -> split (my-malloc.c:180) -> my_realloc (my-malloc.c:458, nhánh shrink) -> test_fuzz_mixed_ops_seeded`.

**Nguyên nhân:** `split()` gọi `list_unlink(&block->list)` một cách vô điều kiện ở cuối hàm, giả định `block` luôn đang nằm trong một free bin. Nhưng ở nhánh shrink của `my_realloc` (`split(current_block, request_size)`), `current_block` là block **đang được cấp phát cho user**, chưa bao giờ nằm trong bin nào — `list.next`/`list.prev` của nó vẫn là byte rác/zero chưa từng được `list_init()`. `list_unlink`'s guard (`if (list_is_linked(node))`) không phân biệt được "chưa từng init" với "đã init nhưng có neighbor thật" nếu next/prev khác `NULL`-self-loop.

**Fix đã áp dụng:** bỏ dòng `list_unlink(&block->list);` ra khỏi `split()`. Đã kiểm chứng: mọi caller (`my_malloc`, `my_realloc` shrink path, `try_expand`) đều đã tự chịu trách nhiệm unlink (hoặc block chưa từng được link) trước khi gọi `split()`.

---

## 2. Bug lộ ra sau fix #1 (ĐÃ SỬA): invariant "allocated-never-on-list" fail

**Triệu chứng:** `CHECK(invariant_ok, "every live allocation is off the free list...")` fail. GDB xác nhận: `flags=0` (đúng là allocated) nhưng `list = {next=0x0, prev=0x0}` — chưa bao giờ `list_init()`.

**Nguyên nhân:** Khi `my_malloc()` (nhánh carve-from-top) hoặc `try_expand()` (nhánh absorb-into-top) tạo ra một `gm.topchunkptr` **mới** bằng `BLOCK_NEXT_HEADER(...)`, header mới này không bao giờ được `list_init()`. Lần sau khi chunk đó bị carve thành block cấp phát, `list` field của nó vẫn là rác/zero → trip `list_is_linked()`.

**Fix đã áp dụng:** thêm `list_init(&gm.topchunkptr->list);` ngay sau khi thiết lập `gm.topchunkptr` mới, ở cả hai chỗ: nhánh carve-from-top trong `my_malloc()` và nhánh absorb-into-top trong `try_expand()`.

---

## 3. Design smell (CHƯA QUYẾT ĐỊNH): double-unlink trong `find_suitable_block` + `my_malloc`

`find_suitable_block()` đã tự `list_unlink()` block trước khi trả về `my_malloc()`. Trước fix #1, `split()` unlink lại lần nữa — vô hại vì `list_unlink()` tự guard bằng `list_is_linked()` + tự `list_init()` lại (self-loop), nên gọi 2 lần trên node đã self-loop là no-op an toàn. Không phải bug thực sự, nhưng là "hai hàm cùng nghĩ mình chịu trách nhiệm unlink" — nên cân nhắc dọn lại nếu có thời gian. **Chưa sửa.**

---

## 4. `try_expand()` — các vấn đề đã tìm & đề xuất tách hàm (CHƯA ÁP DỤNG)

### 4a. Bug so sánh sai đại lượng (điều kiện grow_top)
`if (new_payload >= gm.topsize)` so sai — phải so `needed` (phần thực sự còn thiếu = `new_payload - curr->payload`) với `gm.topsize`, không phải `new_payload` tuyệt đối. Hậu quả: gọi `sbrk()` (qua `grow_top()`) nhiều hơn cần thiết. Không unsafe, chỉ lãng phí. **Đề xuất fix:** tính `needed` trước, so `needed >= gm.topsize`.

### 4b. Non-atomic mutation khi expand thất bại
Nếu forward-merge chạy (unlink `next`, tăng `curr->payload`) nhưng vẫn không đủ, và backward-merge cũng thất bại, hàm trả `NULL` nhưng `curr` đã bị mutate vĩnh viễn. Đã verify: KHÔNG gây corruption (caller fallback `malloc+copy+free` vẫn hoạt động đúng vì block sau merge vẫn well-formed), chỉ lãng phí công sức merge trước khi bỏ. **Đề xuất fix:** tính `best_case` (tổng nếu gộp cả 2 phía) TRƯỚC khi đụng vào bất cứ con trỏ nào; trả `NULL` sớm nếu không đủ.

### 4c. Đề xuất tách 3 case thành 3 hàm `static inline` riêng
Lý do: mỗi case có precondition/invariant riêng (giống style doc-comment đã có sẵn trong `list.h`). Đề xuất:
- `try_expand_into_top(curr, new_payload)` — case top-chunk, có kèm guard `if (new_payload <= curr->payload) return curr;` (tránh underflow `size_t` nếu new_payload nhỏ hơn payload hiện tại — **fix tốt do người dùng tự nghĩ ra**, chưa có trong bản gốc).
- `try_expand_forward(curr, next, new_payload)` — merge-tới, luôn mutate `curr` khi được gọi (precondition: `next` đang free).
- `try_expand_backward(curr, prev)` — merge-ngược/ba-chiều, luôn `memmove` (precondition: `prev` đang free và đủ lớn).

⚠️ **Lưu ý quan trọng — bug regression khi áp dụng:** một bản paste nháp đã vô tình lồng nhầm code merge-ngược **vào bên trong** `if (next_free) { ... }`, khiến case merge-ngược-một-mình (`test_try_expand_backward_only`) không bao giờ chạy được nữa. Khi áp dụng bản tách hàm, PHẢI đảm bảo `try_expand_backward()` được gọi **độc lập**, không lồng trong nhánh `next_free`. Bản code mẫu đã viết đúng thứ tự này ở tin nhắn trước, dùng làm tham chiếu khi áp dụng thật.

**Trạng thái: đã có bản mẫu đầy đủ 3 hàm + hàm điều phối, nhưng CHƯA ÁP DỤNG vào file thật.**

---

## 5. Test bug: `test_try_expand_three_way_no_ghost_node` (ĐỀ XUẤT, chưa xác nhận áp dụng)

**Triệu chứng:** `[FAIL] 'e' is the sole entry in its bin` (`test-edge-cases.c:1079`).

**Nguyên nhân (KHÔNG PHẢI bug allocator):** `e` là block cuối cùng trong 5 block `a,b,c,d,e` được malloc liên tiếp — nên hàng xóm phía trước của nó (sau khi carve) chính là top chunk. Khi `my_free(e)`, `coalesce()` đi vào nhánh absorb-into-top (`next == gm.topchunkptr`), khiến `e` biến thành `gm.topchunkptr` MỚI thay vì được chèn vào bin nhỏ như test kỳ vọng. `bin_holds_exactly()` sau đó tính `get_bin(e->payload)` trên payload đã bị đổi thành số khổng lồ (do absorb), tra nhầm bin, luôn fail.

**Đề xuất fix:** thêm một allocation "chặn" ngay sau `e`:
```c
void *e = my_malloc(48);
void *guard = my_malloc(align); /* blocks e from being absorbed into top chunk when freed */
...
my_free(guard); // free ở cuối hàm luôn
```
(mirror đúng pattern `guard1`/`guard2` đã dùng trong `test_split_threshold`). **Chưa áp dụng vào file test.**

---

## 6. Test bug: `test_heap_shrink_boundary` (ĐANG CÂN NHẮC HƯỚNG SỬA, chưa chọn)

**Vấn đề:** test capture `heap_floor = sbrk(0)` cục bộ TRONG hàm test (sau khi `heap_init()` đã chạy), không phải mốc break thật sự ban đầu của tiến trình. Vì `gm.topchunkptr` có thể "trôi ngược" về gần `heap_start` sau khi mọi thứ được coalesce hết (hành vi ĐÚNG của allocator), test cũ luôn có thể fail dù allocator không hề sai.

**Đã research cách các allocator thật làm** (dlmalloc, glibc):
- dlmalloc có API công khai `malloc_footprint()` / `malloc_max_footprint()` — allocator TỰ báo cáo số byte đã xin từ OS, thay vì để code ngoài tự soi `sbrk(0)`.
- glibc `malloc_trim(3)`: chỉ cam kết trả về 1/0 (có giải phóng hay không), KHÔNG cam kết giải phóng chính xác bao nhiêu byte — không có tài liệu nào nói nên test bằng cách so địa chỉ break tuyệt đối.

**3 hướng đề xuất (chưa chọn hướng nào, chưa áp dụng):**
1. **(Khuyến nghị)** Thêm `my_malloc_footprint()` kiểu dlmalloc, test dựa trên con số này thay vì `sbrk(0)` trực tiếp — tách test khỏi chi tiết triển khai nội bộ (`gm.topchunkptr`, `TOP_PAD_SIZE`), bền hơn nếu sau này đổi cơ chế cấp phát.
2. Self-consistency check: so `heap_final` với đúng công thức `topchunkptr + HEADER_SIZE + TOP_PAD_SIZE` mà `my_free()` dùng.
3. Chỉ test "có trim hay không" (`heap_final < heap_grown`), không cố định ngưỡng dưới tuyệt đối — đúng tinh thần cam kết yếu của `malloc_trim(3)`.
4. Bỏ hẳn test.

---

## 7. Compiler warnings (chưa xác nhận đã fix)

- `-Wimplicit-fallthrough` trong `test_fuzz_mixed_ops_seeded`'s switch (case 2 → default): fallthrough có chủ đích, cần thêm `[[fallthrough]];` (hoặc `__attribute__((fallthrough));`) ngay trước `default:`.
- `'tests' defined but not used`: chỉ là hệ quả của việc tạm thời hijack `main()` để repro không cần fork — sẽ tự hết khi revert `main()` về bản gốc (fork-loop qua `tests[]`).

---

## 8. Quy trình debug đã thiết lập (để tái sử dụng)

Vì test harness chính (`main()`) fork một child process cho MỖI test suite → GDB khó theo dõi trực tiếp. Cách đã dùng để repro riêng lẻ một test (đặc biệt là fuzz test có seed từ `time(NULL)`):

1. Lấy seed thật từ output đã chạy (dòng `seed = %u -- rerun with this seed...`).
2. **Tạm thời** thay body của `main()` trong `test-edge-cases.c`: giữ `enable_color_if_tty()`, `setvbuf(...)`, `heap_init()`, khởi tạo `counters` qua `mmap`, rồi gọi thẳng hàm test cần debug (vd `test_fuzz_mixed_ops_seeded(SEED)`), KHÔNG fork.
3. Build lại với đúng flags cũ (ASan, `-g`), chạy `gdb ./test-edge-cases`, `b <hàm nghi ngờ>`, `run`, `c` qua các lần dừng vô hại, đọc `bt` + `p *block`/`p block->list` khi crash/dừng đúng chỗ.
4. **Nhớ revert `main()` về bản gốc (vòng lặp fork qua `tests[]`)** trước khi coi là "đã sửa xong", chạy lại toàn bộ 31 suites để xác nhận không có regression.

---

## Việc còn tồn đọng (checklist)

- [ ] Sửa `if (new_payload >= gm.topsize)` → so `needed` (4a)
- [ ] Thêm `best_case` check trước khi mutate trong `try_expand` (4b)
- [ ] Áp dụng bản tách 3 hàm `try_expand_into_top/forward/backward` — nhớ tránh bug lồng nhầm (4c)
- [ ] Thêm guard allocation sau `e` trong `test_try_expand_three_way_no_ghost_node` (5)
- [ ] Chọn hướng sửa `test_heap_shrink_boundary` và áp dụng (6)
- [ ] Thêm `[[fallthrough]];` trong `test_fuzz_mixed_ops_seeded` (7)
- [ ] Revert `main()` về bản gốc (fork-loop), chạy lại full suite, xác nhận 135/135 + không regression mới
- [ ] (Tuỳ chọn) Dọn double-unlink smell giữa `find_suitable_block` và `split` (3)