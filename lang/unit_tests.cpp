#include "tlang.h"
#include <taichi/common/testing.h>
#include <taichi/util.h>
#include <taichi/visual/gui.h>
#include <taichi/common/bit.h>

TLANG_NAMESPACE_BEGIN

TC_TEST("select") {
  int n = 128;
  Program prog(Arch::x86_64);

  auto a = var<float32>();
  auto i = ind();

  layout([&]() { root.fixed(i, n).place(a); });

  auto func = kernel(a, [&]() {
    a[i] = select(cmp_ne(imm(0), i % imm(2)), cast<float32>(i), imm(0.0_f));
  });

  func();

  for (int i = 0; i < n; i++) {
    TC_ASSERT(a.val<float32>(i) == (i % 2) * i);
  }
}

TC_TEST("test_snode") {
  Program prog(Arch::x86_64);

  auto i = Expr::index(0);
  auto u = variable(DataType::i32);

  int n = 128;

  // All data structure originates from a "root", which is a forked node.
  prog.layout([&] { root.fixed(i, n).place(u); });

  for (int i = 0; i < n; i++) {
    u.val<int32>(i) = i + 1;
  }

  for (int i = 0; i < n; i++) {
    TC_CHECK_EQUAL(u.val<int32>(i), i + 1, 0);
  }
}

TC_TEST("test_2d_blocked_array") {
  int n = 32, block_size = 16;
  TC_ASSERT(n % block_size == 0);

  Program prog(Arch::x86_64);
  bool forked = false;

  auto a = var<int32>(), b = var<int32>(), i = ind(), j = ind();

  layout([&] {
    if (!forked)
      root.fixed({i, j}, {n / block_size, n * 2 / block_size})
          .fixed({i, j}, {block_size, block_size})
          .forked()
          .place(a, b);
    else {
      root.fixed({i, j}, {n, n * 2}).forked().place(a);
      root.fixed({i, j}, {n, n * 2}).forked().place(b);
    }
  });

  auto inc = kernel(a, [&]() { b[i, j] = a[i, j] + i; });

  for (int i = 0; i < n; i++) {
    for (int j = 0; j < n * 2; j++) {
      a.val<int32>(i, j) = i + j * 3;
    }
  }

  inc();

  for (int i = 0; i < n; i++) {
    for (int j = 0; j < n * 2; j++) {
      TC_ASSERT_EQUAL(b.val<int32>(i, j), i * 2 + j * 3, 0);
      TC_ASSERT_EQUAL(a.val<int32>(i, j), i + j * 3, 0);
    }
  }
}

TC_TEST("test_2d_array") {
  int n = 8;
  Program prog(Arch::x86_64);
  bool forked = true;

  auto a = var<int32>(), b = var<int32>(), i = ind(), j = ind();

  layout([&] {
    if (!forked)
      root.fixed({i, j}, {n, n * 2}).forked().place(a, b);
    else {
      root.fixed({i, j}, {n, n * 2}).forked().place(a);
      root.fixed({i, j}, {n, n * 2}).forked().place(b);
    }
  });

  auto inc = kernel(a, [&]() { b[i, j] = a[i, j] + i; });

  for (int i = 0; i < n; i++) {
    for (int j = 0; j < n * 2; j++) {
      a.val<int32>(i, j) = i + j * 3;
    }
  }

  inc();

  for (int i = 0; i < n; i++) {
    for (int j = 0; j < n * 2; j++) {
      TC_CHECK_EQUAL(a.val<int32>(i, j), i + j * 3, 0);
      TC_CHECK_EQUAL(b.val<int32>(i, j), i * 2 + j * 3, 0);
    }
  }
}

TC_TEST("test_single_program") {
  int n = 128;
  Program prog(Arch::x86_64);

  auto a = var<float32>(), b = var<float32>();
  auto i = ind(0);

  bool fork = true;

  layout([&] {
    if (fork) {
      root.fixed(i, n).forked().place(a, b);
    } else {
      root.fixed(i, n).place(a);
      root.fixed(i, n).place(b);
    }
  });

  auto func1 = kernel(a, [&] { b[i] = a[i] + imm(1.0_f); });

  for (int i = 0; i < n; i++) {
    a.val<float32>(i) = i;
  }

  func1();

  for (int i = 0; i < n; i++) {
    TC_CHECK_EQUAL(b.val<float32>(i), i + 1.0_f, 1e-5_f);
  }
}

TC_TEST("test_multiple_programs") {
  int n = 128;
  Program prog(Arch::x86_64);

  Real a, b, c, d;
  a = placeholder(DataType::f32);
  b = placeholder(DataType::f32);
  c = placeholder(DataType::f32);
  d = placeholder(DataType::f32);

  auto i = Expr::index(0);

  layout([&]() {
    root.fixed(i, n).place(a);
    root.fixed(i, n).place(b);
    root.fixed(i, n).place(c);
    root.fixed(i, n).place(d);
  });

  auto func1 = kernel(a, [&]() { b[i] = a[i] + imm(1.0_f); });
  auto func2 = kernel(a, [&]() { c[i] = b[i] + imm(1.0_f); });
  auto func3 = kernel(a, [&]() { d[i] = c[i] + imm(1.0_f); });

  for (int i = 0; i < n; i++) {
    a.val<float32>(i) = i;
  }

  func1();
  func2();
  func3();

  for (int i = 0; i < n; i++) {
    TC_CHECK_EQUAL(d.val<float32>(i), i + 3.0_f, 1e-5_f);
  }
}

TC_TEST("slp") {
  Program prog;

  int n = 32;
  auto a = var<float32>(), b = var<float32>();

  auto i = ind();

  layout([&] { root.fixed(i, n).forked().place(a, b); });

  for (int i = 0; i < n; i++) {
    a.val<float32>(i) = i;
    b.val<float32>(i) = i + 1;
  }

  auto func = kernel(a, [&]() {
    a[i] = a[i] + imm(1.0_f);
    b[i] = b[i] + imm(2.0_f);

    group(2);
  });

  func();

  for (int i = 0; i < n; i++) {
    TC_CHECK(a.val<float32>(i) == i + 1);
    TC_CHECK(b.val<float32>(i) == i + 3);
  }
}

// a * b * vec

TC_TEST("adapter1") {
  for (auto vec_size : {1, 2, 4, 8, 16}) {
    Program prog;

    Float a, b;
    Vector v(vec_size), u(vec_size);

    int n = 128;
    auto ind = Expr::index(0);

    layout([&] {
      a = var<float32>();
      b = var<float32>();
      root.fixed(ind, n).place(a, b);
      for (int i = 0; i < vec_size; i++) {
        v(i) = var<float32>();
        root.fixed(ind, n).place(v(i));
      }
    });

    auto func = kernel(a, [&]() {
      auto &ad = adapter(0);
      auto ab = a[ind] * b[ind];

      ad.set(1);
      ad.convert(ab);

      for (int d = 0; d < vec_size; d++) {
        v(d)[ind] = ab * v(d)[ind];
      }

      parallel_instances(8);
      group(vec_size);
    });

    for (int i = 0; i < n; i++) {
      a.val<float32>(i) = i;
      b.val<float32>(i) = 2.0_f * (i + 1);
      for (int j = 0; j < vec_size; j++) {
        v(j).val<float32>(i) = 1.0_f * j / (i + 1);
      }
    }

    func();

    for (int i = 0; i < n; i++) {
      for (int j = 0; j < vec_size; j++) {
        auto val = v(j).val<float32>(i);
        float32 gt = i * j * 2;
        TC_CHECK_EQUAL(gt, val, 1e-3_f);
      }
    }
  }
}

// Vec<vec_size> reduction
TC_TEST("adapter2") {
  int n = 64;

  for (auto vec_size : {1, 2, 4, 8, 16}) {
    Program prog;

    Vector v(vec_size);

    Float sum;

    auto ind = Expr::index(0);

    layout([&] {
      for (int i = 0; i < vec_size; i++) {
        v(i) = var<float32>();
        root.fixed(ind, n).place(v(i));
      }
      sum = var<float32>();
      root.fixed(ind, n).place(sum);
    });

    auto func = kernel(sum, [&] {
      auto v_ind = v[ind];

      for (int i = 0; i < vec_size; i++) {
        v_ind(i).set(load(v_ind(i)));
      }

      auto &ad = adapter(0);
      ad.set(vec_size);
      for (int i = 0; i < vec_size; i++) {
        ad.convert(v_ind(i));
      }

      Expr acc = Expr::create_imm(0.0_f);
      for (int d = 0; d < vec_size; d++) {
        acc = acc + v_ind(d);
      }

      sum[ind] = acc;

      parallel_instances(8);
      group(1);
    });

    for (int i = 0; i < n; i++) {
      for (int j = 0; j < vec_size; j++) {
        v(j).val<float32>(i) = j + i;
      }
    }

    func();

    for (int i = 0; i < n; i++) {
      auto val = sum.val<float32>(i);
      float32 gt = vec_size * (vec_size - 1) / 2 + i * vec_size;
      TC_CHECK_EQUAL(gt, val, 1e-5_f);
    }
  }
}

// reduce(vec_a<n> - vec_b<n>) * vec_c<2n>
TC_TEST("adapter3") {
  for (auto vec_size : {1, 2, 4, 8}) {
    // why vec_size = 16 fails??
    Program prog;

    Vector a(vec_size), b(vec_size), c(vec_size * 2);
    Float sum;

    int n = 64;

    auto ind = Expr::index(0);

    layout([&] {
      for (int i = 0; i < vec_size; i++) {
        a(i) = var<float32>();
        root.fixed(ind, n).place(a(i));
        b(i) = var<float32>();
        root.fixed(ind, n).place(b(i));
      }

      for (int i = 0; i < vec_size * 2; i++) {
        c(i) = var<float32>();
        root.fixed(ind, n).place(c(i));
      }
    });

    auto func = kernel(a(0), [&]() {
      auto aind = a[ind];
      auto bind = b[ind];
      auto cind = c[ind];

      auto diff = aind.element_wise_prod(aind) - bind.element_wise_prod(bind);

      {
        auto &ad = adapter(0);
        ad.set(vec_size);
        for (int i = 0; i < vec_size; i++)
          ad.convert(diff(i));
      }

      auto acc = Expr::create_imm(0.0_f);
      for (int d = 0; d < vec_size; d++) {
        acc = acc + diff(d);
      }

      {
        auto &ad = adapter(1);
        ad.set(1);
        ad.convert(acc);
        for (int i = 0; i < vec_size * 2; i++)
          c(i)[ind] = c(i)[ind] * acc;
      }

      for (int i = 0; i < n; i++) {
        for (int j = 0; j < vec_size; j++) {
          a(j).val<float32>(i) = i + j + 1;
          b(j).val<float32>(i) = i + j;
        }
        for (int j = 0; j < vec_size * 2; j++) {
          c(j).val<float32>(i) = i - 2 + j;
        }
      }
      group(vec_size * 2);
      parallel_instances(8);
    });

    func();

    for (int i = 0; i < n; i++) {
      real s = 0;
      for (int j = 0; j < vec_size; j++) {
        s += taichi::sqr(i + j + 1) - taichi::sqr(i + j);
      }
      for (int j = 0; j < vec_size * 2; j++) {
        auto val = c(j).val<float32>(i);
        auto gt = s * (i - 2 + j);
        TC_CHECK_EQUAL(gt, val, 1e-3_f);
      }
    }
  }
}

TLANG_NAMESPACE_END
