#include "UnitTest.h"
#include "pascalrt.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

TEST(RuntimeAnsi, StringOperations) {
  EXPECT_TRUE(__pas_astr_from_chars("abc", 0) == nullptr);
  EXPECT_TRUE(__pas_astr_from_chars("abc", -3) == nullptr);
  EXPECT_EQ(__pas_astr_length(nullptr), 0);

  char *a = __pas_astr_from_cstr("hello");
  char *b = __pas_astr_from_cstr("world");
  EXPECT_EQ(__pas_astr_length(a), 5);
  EXPECT_EQ(std::string(a), "hello");

  /* __pas_astr_join handles negative n safely without underflow */
  char *joinedNeg = __pas_astr_join(a, "ignored", -10, b);
  EXPECT_EQ(std::string(joinedNeg), "helloworld");

  char *joined = __pas_astr_join(a, ", ", 2, b);
  EXPECT_EQ(std::string(joined), "hello, world");
  EXPECT_EQ(__pas_astr_pos(__pas_astr_from_cstr("world"), joined), 8);

  /* Copy-on-write and self-append */
  char *shared = nullptr;
  __pas_astr_assign(&shared, a);
  char *alias = nullptr;
  __pas_astr_assign(&alias, shared);
  __pas_astr_append(&shared, shared);
  EXPECT_EQ(std::string(shared), "hellohello");
  EXPECT_EQ(std::string(alias), "hello");

  /* Insert & delete */
  char *ins = __pas_astr_from_cstr("!");
  __pas_astr_insert(ins, &shared, 6);
  EXPECT_EQ(std::string(shared), "hello!hello");
  __pas_astr_delete(&shared, 6, 1);
  EXPECT_EQ(std::string(shared), "hellohello");

  /* Append & prepend in-place growth, CoW, and self-prepend */
  char *built = nullptr;
  __pas_astr_append_char(&built, 'b');
  __pas_astr_prepend_char(&built, 'a');
  for (int i = 0; i < 20; ++i)
    __pas_astr_prepend_char(&built, static_cast<char>('0' + (i % 10)));
  EXPECT_EQ(__pas_astr_length(built), 22);
  EXPECT_EQ(std::string(built), "98765432109876543210ab");
  __pas_astr_prepend(&built, a);
  EXPECT_EQ(std::string(built), "hello98765432109876543210ab");

  char *builtAlias = nullptr;
  __pas_astr_assign(&builtAlias, built);
  __pas_astr_prepend_char(&built, '^');
  EXPECT_EQ(std::string(built), "^hello98765432109876543210ab");
  EXPECT_EQ(std::string(builtAlias), "hello98765432109876543210ab");
  __pas_astr_prepend(&built, built);
  EXPECT_EQ(std::string(built),
            "^hello98765432109876543210ab^hello98765432109876543210ab");

  /* SetLength grow and shrink */
  __pas_astr_setlength(&built, 60);
  EXPECT_EQ(__pas_astr_length(built), 60);
  EXPECT_EQ(built[56], '\0');
  __pas_astr_setlength(&built, 4);
  EXPECT_EQ(std::string(built), "^hel");

  /* Coalesced multi-part append helpers (2-part, N-part, CoW, self-alias) */
  char *multi = nullptr;
  __pas_astr_append_sc(&multi, a, ' ');
  __pas_astr_append_cs(&multi, '[', b);
  __pas_astr_append2(&multi, a, b);
  EXPECT_EQ(std::string(multi), "hello [worldhelloworld");

  char *multiAlias = nullptr;
  __pas_astr_assign(&multiAlias, multi);
  const char *pPtrs[3] = {"!", multi, "?"};
  int64_t pLens[3] = {1, -1, 1};
  __pas_astr_append_n(&multi, pPtrs, pLens, 3);
  EXPECT_EQ(std::string(multiAlias), "hello [worldhelloworld");
  EXPECT_EQ(std::string(multi),
            "hello [worldhelloworld!hello [worldhelloworld?");

  /* Self-alias with refs == 1 */
  __pas_astr_clear(&multiAlias);
  __pas_astr_append_sc(&multi, multi, '!');
  EXPECT_EQ(std::string(multi),
            "hello [worldhelloworld!hello [worldhelloworld?"
            "hello [worldhelloworld!hello [worldhelloworld?!");

  __pas_astr_clear(&multi);
  /* Small-string pool reuse, eviction past cap, realloc growth, and shrink */
  std::vector<char *> poolKeep;
  for (int i = 0; i < 48; ++i)
    poolKeep.push_back(__pas_astr_from_cstr("tiny"));
  for (char *&p : poolKeep)
    __pas_astr_clear(&p);
  char *reused = __pas_astr_from_cstr("short");
  char *tail = __pas_astr_from_cstr("012345678901234567890123456789");
  __pas_astr_append(&reused, tail);
  EXPECT_EQ(__pas_astr_length(reused), 35);
  __pas_astr_setlength(&reused, 3);
  EXPECT_EQ(std::string(reused), "sho");
  __pas_astr_clear(&tail);
  __pas_astr_clear(&reused);
  char *mid = __pas_astr_from_cstr("twenty_chars_long_12");
  __pas_astr_clear(&mid);

  __pas_astr_clear(&built);
  __pas_astr_clear(&builtAlias);
  __pas_astr_clear(&a);
  __pas_astr_clear(&b);
  __pas_astr_clear(&joinedNeg);
  __pas_astr_clear(&joined);
  __pas_astr_clear(&shared);
  __pas_astr_clear(&alias);
  __pas_astr_clear(&ins);
}

TEST(RuntimeAnsi, DynamicArrayOperations) {
  static const PasDynInfo Info = {sizeof(int64_t), nullptr, nullptr};
  void *empty = __pas_dyn_new(&Info, 0);
  EXPECT_TRUE(empty == nullptr);
  EXPECT_EQ(__pas_dyn_length(empty), 0);

  void *arr = nullptr;
  __pas_dyn_setlength(&arr, &Info, 3);
  EXPECT_EQ(__pas_dyn_length(arr), 3);
  auto *p = static_cast<int64_t *>(arr);
  EXPECT_EQ(p[0], 0);
  p[0] = 11;
  p[1] = 22;
  p[2] = 33;

  /* Negative count with negative start must safely return NULL without UB */
  EXPECT_TRUE(__pas_dyn_copy(arr, -5, INT64_MIN) == nullptr);
  EXPECT_TRUE(__pas_dyn_copy(arr, -1, 2) != nullptr);
  void *sub = __pas_dyn_copy(arr, 1, 2);
  EXPECT_EQ(__pas_dyn_length(sub), 2);
  EXPECT_EQ(static_cast<int64_t *>(sub)[0], 22);
  EXPECT_EQ(static_cast<int64_t *>(sub)[1], 33);

  /* Copy-on-write on shared array SetLength */
  void *alias = nullptr;
  __pas_dyn_assign(&alias, arr);
  __pas_dyn_setlength(&arr, &Info, 5);
  EXPECT_EQ(__pas_dyn_length(arr), 5);
  EXPECT_EQ(static_cast<int64_t *>(arr)[3], 0);
  EXPECT_EQ(__pas_dyn_length(alias), 3);
  EXPECT_EQ(static_cast<int64_t *>(alias)[2], 33);

  __pas_dyn_clear(&sub);
  __pas_dyn_clear(&alias);
  __pas_dyn_clear(&arr);
  EXPECT_TRUE(arr == nullptr);
}

TEST(RuntimeFormat, BasicFormatting) {
  char *fmt = __pas_astr_from_cstr("%s:%d:%.2f:%d:%x");
  char *sval = __pas_astr_from_cstr("ok");
  double dval = 3.5;
  int64_t i64val = -9000000000000LL;

  PasVarRec args[6];
  std::memset(args, 0, sizeof(args));
  args[0].vtype = 5;
  args[1].vtype = 11; /* VtAnsiString */
  args[1].v.pointer = sval;
  args[2].vtype = 0; /* VtInteger */
  args[2].v.integer = 42;
  args[3].vtype = 3; /* VtExtended */
  args[3].v.real = &dval;
  args[4].vtype = 16; /* VtInt64 */
  args[4].v.int64 = &i64val;
  args[5].vtype = 0; /* VtInteger */
  args[5].v.integer = 255;

  char *out = __pas_format(fmt, args);
  EXPECT_EQ(std::string(out), "ok:42:3.50:-9000000000000:FF");

  __pas_astr_clear(&out);
  __pas_astr_clear(&sval);
  __pas_astr_clear(&fmt);
}

TEST(RuntimeSys, FilesAndThreadSafeStrerror) {
  llvm::SmallString<128> Dir;
  EXPECT_FALSE(llvm::sys::fs::createUniqueDirectory("p3-rt-test", Dir));
  llvm::SmallString<128> Sub(Dir);
  llvm::sys::path::append(Sub, "nested");
  EXPECT_EQ(__pas_sys_mkdir(Sub.c_str()), 1);
  EXPECT_EQ(__pas_sys_dir_exists(Sub.c_str()), 1);
  EXPECT_EQ(__pas_sys_file_exists(Sub.c_str()), 0);
  EXPECT_EQ(__pas_sys_rmdir(Sub.c_str()), 1);
  EXPECT_EQ(__pas_sys_dir_exists(Sub.c_str()), 0);
  llvm::sys::fs::remove(Dir);

  double v = 0.0;
  EXPECT_EQ(__pas_sys_parse_double("12.25", &v), 1);
  EXPECT_EQ(v, 12.25);
  EXPECT_EQ(__pas_sys_parse_double("not-a-num", &v), 0);

  std::vector<std::thread> threads;
  std::atomic<int> failures{0};
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&]() {
      for (int i = 0; i < 100; ++i) {
        errno = ENOENT;
        const char *msg = __pas_sys_strerror();
        if (!msg || !msg[0])
          ++failures;
      }
    });
  }
  for (auto &th : threads)
    th.join();
  EXPECT_EQ(failures.load(), 0);
}

TEST(RuntimeIO, TextFileRoundtrip) {
  PasFile *f = __pas_file_new(1, 1, nullptr);
  __pas_rewrite(f);
  __pas_write_begin(f);
  __pas_write_int(f, -9223372036854775807LL, 0);
  __pas_write_char(f, ' ', 0);
  __pas_write_uint(f, 18446744073709551615ULL, 0);
  __pas_writeln(f);
  __pas_write_end(f);

  __pas_reset(f);
  EXPECT_EQ(__pas_read_int64(f), -9223372036854775807LL);
  EXPECT_EQ(__pas_read_uint64(f), 18446744073709551615ULL);
  __pas_readln(f);
  EXPECT_EQ(__pas_eof(f), 1);
  __pas_close(f);
  __pas_file_free(f);
}
