#include "bench.h"
#include "tpcc.h"

// configuration flags
extern int g_disable_xpartition_txn;
extern int g_enable_separate_tree_per_partition;
extern int g_new_order_remote_item_pct;
extern int g_new_order_fast_id_gen;
extern int g_uniform_item_dist;
extern int g_order_status_scan_hack;
extern int g_wh_temperature;
extern uint g_microbench_rows;  // this many rows
// can't have both ratio and rows at the same time
extern int g_microbench_wr_rows;  // this number of rows to write
extern int g_nr_suppliers;

// how much % of time a worker should use a random home wh
// 0 - always use home wh
// 50 - 50% of time use random wh
// 100 - always use a random wh
extern double g_wh_spread;

// TPC-C workload mix
// 0: NewOrder
// 1: Payment
// 2: CreditCheck
// 3: Delivery
// 4: OrderStatus
// 5: StockLevel
// 6: TPC-CH query 2 variant - original query 2, but /w marginal stock table
// update
// 7: Microbenchmark-random - same as Microbenchmark, but uses random read-set
// range
static unsigned g_txn_workload_mix[] = {
    45, 43, 0, 4, 4, 4, 0, 0};  // default TPC-C workload mix

extern util::aligned_padded_elem<std::atomic<uint64_t>> *g_district_ids ;

typedef std::vector<std::vector<std::pair<int32_t, int32_t>>> SuppStockMap;
extern SuppStockMap supp_stock_map;

// config constants
struct Nation {
  int id;
  std::string name;
  int rId;
};
extern const Nation nations[];

extern const char *regions[];

static constexpr ALWAYS_INLINE size_t NumItems() { return 100000; }

static constexpr ALWAYS_INLINE size_t NumDistrictsPerWarehouse() { return 10; }

static constexpr ALWAYS_INLINE size_t NumCustomersPerDistrict() { return 3000; }

static ALWAYS_INLINE size_t NumWarehouses() {
  return (size_t)ermia::config::benchmark_scale_factor;
}

static inline std::atomic<uint64_t> &NewOrderIdHolder(unsigned warehouse,
                                                 unsigned district) {
  ASSERT(warehouse >= 1 && warehouse <= NumWarehouses());
  ASSERT(district >= 1 && district <= NumDistrictsPerWarehouse());
  const unsigned idx =
      (warehouse - 1) * NumDistrictsPerWarehouse() + (district - 1);
  return g_district_ids[idx].elem;
}

static inline uint64_t FastNewOrderIdGen(unsigned warehouse,
                                         unsigned district) {
  return NewOrderIdHolder(warehouse, district)
      .fetch_add(1, std::memory_order_acq_rel);
}

struct eqstr {
  bool operator()(const char *s1, const char *s2) const {
    return (s1 == s2) || (s1 && s2 && strcmp(s1, s2) == 0);
  }
};

#ifndef NDEBUG
struct checker {
  // these sanity checks are just a few simple checks to make sure
  // the data is not entirely corrupted

  static ALWAYS_INLINE void SanityCheckCustomer(
      const customer::key *k, const customer::value *v) {
    ASSERT(v->c_credit == "BC" || v->c_credit == "GC");
    ASSERT(v->c_middle == "OE");
    ASSERT(k->c_w_id >= 1 && static_cast<size_t>(k->c_w_id) <= NumWarehouses());
    ASSERT(k->c_d_id >= 1 &&
           static_cast<size_t>(k->c_d_id) <= NumDistrictsPerWarehouse());
    ASSERT(k->c_id >= 1 &&
           static_cast<size_t>(k->c_id) <= NumCustomersPerDistrict());
  }

  static ALWAYS_INLINE void SanityCheckWarehouse(
      const warehouse::key *k, const warehouse::value *v) {
    ASSERT(k->w_id >= 1 && static_cast<size_t>(k->w_id) <= NumWarehouses());
    ASSERT(v->w_state.size() == 2);
    ASSERT(v->w_zip == "123456789");
  }

  static ALWAYS_INLINE void SanityCheckDistrict(
      const district::key *k, const district::value *v) {
    ASSERT(k->d_w_id >= 1 && static_cast<size_t>(k->d_w_id) <= NumWarehouses());
    ASSERT(k->d_id >= 1 &&
           static_cast<size_t>(k->d_id) <= NumDistrictsPerWarehouse());
    ASSERT(v->d_next_o_id >= 3001);
    ASSERT(v->d_state.size() == 2);
    ASSERT(v->d_zip == "123456789");
  }

  static ALWAYS_INLINE void SanityCheckItem(const item::key *k,
                                                   const item::value *v) {
    ASSERT(k->i_id >= 1 && static_cast<size_t>(k->i_id) <= NumItems());
    ASSERT(v->i_price >= 1.0 && v->i_price <= 100.0);
  }

  static ALWAYS_INLINE void SanityCheckStock(const stock::key *k) {
    ASSERT(k->s_w_id >= 1 && static_cast<size_t>(k->s_w_id) <= NumWarehouses());
    ASSERT(k->s_i_id >= 1 && static_cast<size_t>(k->s_i_id) <= NumItems());
  }

  static ALWAYS_INLINE void SanityCheckNewOrder(const new_order::key *k) {
    ASSERT(k->no_w_id >= 1 &&
           static_cast<size_t>(k->no_w_id) <= NumWarehouses());
    ASSERT(k->no_d_id >= 1 &&
           static_cast<size_t>(k->no_d_id) <= NumDistrictsPerWarehouse());
  }

  static ALWAYS_INLINE void SanityCheckOOrder(const oorder::key *k,
                                                     const oorder::value *v) {
    ASSERT(k->o_w_id >= 1 && static_cast<size_t>(k->o_w_id) <= NumWarehouses());
    ASSERT(k->o_d_id >= 1 &&
           static_cast<size_t>(k->o_d_id) <= NumDistrictsPerWarehouse());
    ASSERT(v->o_c_id >= 1 &&
           static_cast<size_t>(v->o_c_id) <= NumCustomersPerDistrict());
    ASSERT(v->o_carrier_id >= 0 &&
           static_cast<size_t>(v->o_carrier_id) <= NumDistrictsPerWarehouse());
    ASSERT(v->o_ol_cnt >= 5 && v->o_ol_cnt <= 15);
  }

  static ALWAYS_INLINE void SanityCheckOrderLine(
      const order_line::key *k, const order_line::value *v) {
    ASSERT(k->ol_w_id >= 1 &&
           static_cast<size_t>(k->ol_w_id) <= NumWarehouses());
    ASSERT(k->ol_d_id >= 1 &&
           static_cast<size_t>(k->ol_d_id) <= NumDistrictsPerWarehouse());
    ASSERT(k->ol_number >= 1 && k->ol_number <= 15);
    ASSERT(v->ol_i_id >= 1 && static_cast<size_t>(v->ol_i_id) <= NumItems());
  }
};
#endif

class tpcc_table_scanner : public ermia::OrderedIndex::ScanCallback {
 public:
  tpcc_table_scanner(ermia::str_arena *arena) : _arena(arena) {}
  virtual bool Invoke(const char *keyp, size_t keylen, const ermia::varstr &value) {
    ermia::varstr *const k = _arena->next(keylen);
    ermia::varstr *v = _arena->next(0);  // header only
    v->p = value.p;
    v->l = value.l;
    ASSERT(k);
    k->copy_from(keyp, keylen);
    output.emplace_back(k, v);
    return true;
  }

  void clear() { output.clear(); }
  std::vector<std::pair<ermia::varstr *, const ermia::varstr *>> output;
  ermia::str_arena *_arena;
};

class tpcc_worker_mixin : private _dummy {
#define DEFN_TBL_INIT_X(name) , tbl_##name##_vec(partitions.at(#name))

 public:
  tpcc_worker_mixin(const std::map<std::string, std::vector<ermia::OrderedIndex *>> &partitions)
      : _dummy()  // so hacky...
        TPCC_TABLE_LIST(DEFN_TBL_INIT_X) {
    ALWAYS_ASSERT(NumWarehouses() >= 1);
  }

#undef DEFN_TBL_INIT_X

 protected:
#define DEFN_TBL_ACCESSOR_X(name)                                              \
 private:                                                                      \
  std::vector<ermia::OrderedIndex *> tbl_##name##_vec;                         \
                                                                               \
 protected:                                                                    \
  ALWAYS_INLINE ermia::ConcurrentMasstreeIndex *tbl_##name(unsigned int wid) { \
    ASSERT(wid >= 1 && wid <= NumWarehouses());                                \
    ASSERT(tbl_##name##_vec.size() == NumWarehouses());                        \
    return (ermia::ConcurrentMasstreeIndex *)tbl_##name##_vec[wid - 1];        \
  }

  TPCC_TABLE_LIST(DEFN_TBL_ACCESSOR_X)

#undef DEFN_TBL_ACCESSOR_X

 public:
  static inline uint32_t GetCurrentTimeMillis() {
    // struct timeval tv;
    // ALWAYS_ASSERT(gettimeofday(&tv, 0) == 0);
    // return tv.tv_sec * 1000;

    // XXX(stephentu): implement a scalable GetCurrentTimeMillis()
    // for now, we just give each core an increasing number

    static thread_local uint32_t tl_hack = 0;
    return tl_hack++;
  }

  // utils for generating random #s and strings

  static ALWAYS_INLINE int CheckBetweenInclusive(int v, int lower,
                                                        int upper) {
    MARK_REFERENCED(lower);
    MARK_REFERENCED(upper);
    ASSERT(v >= lower);
    ASSERT(v <= upper);
    return v;
  }

  static ALWAYS_INLINE int RandomNumber(util::fast_random &r, int min,
                                               int max) {
    return CheckBetweenInclusive(
        (int)(r.next_uniform() * (max - min + 1) + min), min, max);
  }

  static ALWAYS_INLINE int NonUniformRandom(util::fast_random &r, int A, int C,
                                                   int min, int max) {
    return (((RandomNumber(r, 0, A) | RandomNumber(r, min, max)) + C) %
            (max - min + 1)) +
           min;
  }

  static ALWAYS_INLINE int GetItemId(util::fast_random &r) {
    return CheckBetweenInclusive(
        g_uniform_item_dist ? RandomNumber(r, 1, NumItems())
                            : NonUniformRandom(r, 8191, 7911, 1, NumItems()),
        1, NumItems());
  }

  static ALWAYS_INLINE int GetCustomerId(util::fast_random &r) {
    return CheckBetweenInclusive(
        NonUniformRandom(r, 1023, 259, 1, NumCustomersPerDistrict()), 1,
        NumCustomersPerDistrict());
  }

  static std::string NameTokens[];

  // all tokens are at most 5 chars long
  static const size_t CustomerLastNameMaxSize = 5 * 3;

  static inline size_t GetCustomerLastName(uint8_t *buf, int num) {
    const std::string &s0 = NameTokens[num / 100];
    const std::string &s1 = NameTokens[(num / 10) % 10];
    const std::string &s2 = NameTokens[num % 10];
    uint8_t *const begin = buf;
    const size_t s0_sz = s0.size();
    const size_t s1_sz = s1.size();
    const size_t s2_sz = s2.size();
    memcpy(buf, s0.data(), s0_sz);
    buf += s0_sz;
    memcpy(buf, s1.data(), s1_sz);
    buf += s1_sz;
    memcpy(buf, s2.data(), s2_sz);
    buf += s2_sz;
    return buf - begin;
  }

  static inline std::string GetCustomerLastName(int num) {
    std::string ret;
    ret.resize(CustomerLastNameMaxSize);
    ret.resize(GetCustomerLastName((uint8_t *)&ret[0], num));
    return ret;
  }

  static ALWAYS_INLINE std::string
  GetNonUniformCustomerLastNameLoad(util::fast_random &r) {
    return GetCustomerLastName(NonUniformRandom(r, 255, 157, 0, 999));
  }

  static ALWAYS_INLINE size_t
  GetNonUniformCustomerLastNameRun(uint8_t *buf, util::fast_random &r) {
    return GetCustomerLastName(buf, NonUniformRandom(r, 255, 223, 0, 999));
  }

  static ALWAYS_INLINE size_t
  GetNonUniformCustomerLastNameRun(char *buf, util::fast_random &r) {
    return GetNonUniformCustomerLastNameRun((uint8_t *)buf, r);
  }

  static ALWAYS_INLINE std::string
  GetNonUniformCustomerLastNameRun(util::fast_random &r) {
    return GetCustomerLastName(NonUniformRandom(r, 255, 223, 0, 999));
  }

  // following oltpbench, we really generate strings of len - 1...
  static inline std::string RandomStr(util::fast_random &r, uint len) {
    // this is a property of the oltpbench implementation...
    if (!len) return "";

    uint i = 0;
    std::string buf(len - 1, 0);
    while (i < (len - 1)) {
      const char c = (char)r.next_char();
      // XXX(stephentu): oltpbench uses java's Character.isLetter(), which
      // is a less restrictive filter than isalnum()
      if (!isalnum(c)) continue;
      buf[i++] = c;
    }
    return buf;
  }

  // RandomNStr() actually produces a std::string of length len
  static inline std::string RandomNStr(util::fast_random &r, uint len) {
    const char base = '0';
    std::string buf(len, 0);
    for (uint i = 0; i < len; i++) buf[i] = (char)(base + (r.next() % 10));
    return buf;
  }
};

class tpcc_nation_loader : public bench_loader, public tpcc_worker_mixin {
 public:
  tpcc_nation_loader(unsigned long seed, ermia::Engine *db,
                     const std::map<std::string, ermia::OrderedIndex *> &open_tables,
                     const std::map<std::string, std::vector<ermia::OrderedIndex *>> &partitions)
      : bench_loader(seed, db, open_tables), tpcc_worker_mixin(partitions) {}

 protected:
  virtual void load() {
    std::string obj_buf;
    ermia::transaction *txn = db->NewTransaction(0, *arena, txn_buf());
    uint i;
    for (i = 0; i < 62; i++) {
      const nation::key k(nations[i].id);
      nation::value v;

      const std::string n_comment = RandomStr(r, RandomNumber(r, 10, 20));
      v.n_name = std::string(nations[i].name);
      v.n_regionkey = nations[i].rId;
      v.n_comment.assign(n_comment);
      TryVerifyStrict(tbl_nation(1)->InsertRecord(txn, Encode(str(Size(k)), k),
                                              Encode(str(Size(v)), v)));
    }
    TryVerifyStrict(db->Commit(txn));
  }
};

class tpcc_region_loader : public bench_loader, public tpcc_worker_mixin {
 public:
  tpcc_region_loader(unsigned long seed, ermia::Engine *db,
                     const std::map<std::string, ermia::OrderedIndex *> &open_tables,
                     const std::map<std::string, std::vector<ermia::OrderedIndex *>> &partitions)
      : bench_loader(seed, db, open_tables), tpcc_worker_mixin(partitions) {}

 protected:
  virtual void load() {
    std::string obj_buf;
    ermia::transaction *txn = db->NewTransaction(0, *arena, txn_buf());
    for (uint i = 0; i < 5; i++) {
      const region::key k(i);
      region::value v;

      v.r_name = std::string(regions[i]);
      const std::string r_comment = RandomStr(r, RandomNumber(r, 10, 20));
      v.r_comment.assign(r_comment);
      TryVerifyStrict(tbl_region(1)->InsertRecord(txn, Encode(str(Size(k)), k),
                                              Encode(str(Size(v)), v)));
    }
    TryVerifyStrict(db->Commit(txn));
  }
};

class tpcc_supplier_loader : public bench_loader, public tpcc_worker_mixin {
 public:
  tpcc_supplier_loader(unsigned long seed, ermia::Engine *db,
                       const std::map<std::string, ermia::OrderedIndex *> &open_tables,
                       const std::map<std::string, std::vector<ermia::OrderedIndex *>> &partitions)
      : bench_loader(seed, db, open_tables), tpcc_worker_mixin(partitions) {}

 protected:
  virtual void load() {
    std::string obj_buf;
    for (uint i = 0; i < 10000; i++) {
      ermia::transaction *txn = db->NewTransaction(0, *arena, txn_buf());
      const supplier::key k(i);
      supplier::value v;

      v.su_name = std::string("Supplier#") + std::string("000000000") + std::to_string(i);
      v.su_address = RandomStr(r, RandomNumber(r, 10, 40));

      auto rand = 0;
      while (rand == 0 || (rand > '9' && rand < 'A') ||
             (rand > 'Z' && rand < 'a'))
        rand = RandomNumber(r, '0', 'z');
      v.su_nationkey = rand;
      //		  v.su_phone = std::string("911");			//
      //XXX. nobody wants this field
      //		  v.su_acctbal = 0;
      //		  v.su_comment = RandomStr(r, RandomNumber(r,10,39));
      //// XXX. Q16 uses this. fix this if needed.

      TryVerifyStrict(tbl_supplier(1)->InsertRecord(txn, Encode(str(Size(k)), k),
                                                Encode(str(Size(v)), v)));

      TryVerifyStrict(db->Commit(txn));
    }
  }
};

class tpcc_warehouse_loader : public bench_loader, public tpcc_worker_mixin {
 public:
  tpcc_warehouse_loader(unsigned long seed, ermia::Engine *db,
                        const std::map<std::string, ermia::OrderedIndex *> &open_tables,
                        const std::map<std::string, std::vector<ermia::OrderedIndex *>> &partitions)
      : bench_loader(seed, db, open_tables), tpcc_worker_mixin(partitions) {}

 protected:
  virtual void load() {
    std::string obj_buf;
    uint64_t warehouse_total_sz = 0, n_warehouses = 0;
    std::vector<warehouse::value> warehouses;
    for (uint i = 1; i <= NumWarehouses(); i++) {
      arena->reset();
      ermia::transaction *txn = db->NewTransaction(0, *arena, txn_buf());
      const warehouse::key k(i);

      const std::string w_name = RandomStr(r, RandomNumber(r, 6, 10));
      const std::string w_street_1 = RandomStr(r, RandomNumber(r, 10, 20));
      const std::string w_street_2 = RandomStr(r, RandomNumber(r, 10, 20));
      const std::string w_city = RandomStr(r, RandomNumber(r, 10, 20));
      const std::string w_state = RandomStr(r, 3);
      const std::string w_zip = "123456789";

      warehouse::value v;
      v.w_ytd = 300000;
      v.w_tax = (float)RandomNumber(r, 0, 2000) / 10000.0;
      v.w_name.assign(w_name);
      v.w_street_1.assign(w_street_1);
      v.w_street_2.assign(w_street_2);
      v.w_city.assign(w_city);
      v.w_state.assign(w_state);
      v.w_zip.assign(w_zip);

#ifndef NDEBUG
      checker::SanityCheckWarehouse(&k, &v);
#endif
      const size_t sz = Size(v);
      warehouse_total_sz += sz;
      n_warehouses++;
      TryVerifyStrict(tbl_warehouse(i)->InsertRecord(txn, Encode(str(Size(k)), k),
                                                 Encode(str(sz), v)));

      warehouses.push_back(v);
      TryVerifyStrict(db->Commit(txn));
    }
    for (uint i = 1; i <= NumWarehouses(); i++) {
      arena->reset();
      ermia::transaction *txn = db->NewTransaction(0, *arena, txn_buf());
      const warehouse::key k(i);
      warehouse::value warehouse_temp;
      ermia::varstr warehouse_v;

      rc_t rc = rc_t{RC_INVALID};
      tbl_warehouse(i)->GetRecord(txn, rc, Encode(str(Size(k)), k), warehouse_v);
      TryVerifyStrict(rc);

      const warehouse::value *v = Decode(warehouse_v, warehouse_temp);
      ALWAYS_ASSERT(warehouses[i - 1] == *v);

#ifndef NDEBUG
      checker::SanityCheckWarehouse(&k, v);
#endif
      TryVerifyStrict(db->Commit(txn));
    }

    // pre-build supp-stock mapping table to boost tpc-ch queries
    for (uint w = 1; w <= NumWarehouses(); w++)
      for (uint i = 1; i <= NumItems(); i++)
        supp_stock_map[w * i % 10000].push_back(std::make_pair(w, i));
    if (ermia::config::verbose) {
      std::cerr << "[INFO] finished loading warehouse" << std::endl;
      std::cerr << "[INFO]   * average warehouse record length: "
           << (double(warehouse_total_sz) / double(n_warehouses)) << " bytes"
           << std::endl;
    }
  }
};

class tpcc_item_loader : public bench_loader, public tpcc_worker_mixin {
 public:
  tpcc_item_loader(unsigned long seed, ermia::Engine *db,
                   const std::map<std::string, ermia::OrderedIndex *> &open_tables,
                   const std::map<std::string, std::vector<ermia::OrderedIndex *>> &partitions)
      : bench_loader(seed, db, open_tables), tpcc_worker_mixin(partitions) {}

 protected:
  virtual void load() {
    std::string obj_buf;
    uint64_t total_sz = 0;
    for (uint i = 1; i <= NumItems(); i++) {
      arena->reset();
      ermia::transaction *txn = db->NewTransaction(0, *arena, txn_buf());
      // items don't "belong" to a certain warehouse, so no pinning
      const item::key k(i);

      item::value v;
      const std::string i_name = RandomStr(r, RandomNumber(r, 14, 24));
      v.i_name.assign(i_name);
      v.i_price = (float)RandomNumber(r, 100, 10000) / 100.0;
      const int len = RandomNumber(r, 26, 50);
      if (RandomNumber(r, 1, 100) > 10) {
        const std::string i_data = RandomStr(r, len);
        v.i_data.assign(i_data);
      } else {
        const int startOriginal = RandomNumber(r, 2, (len - 8));
        const std::string i_data = RandomStr(r, startOriginal + 1) + "ORIGINAL" +
                              RandomStr(r, len - startOriginal - 7);
        v.i_data.assign(i_data);
      }
      v.i_im_id = RandomNumber(r, 1, 10000);

#ifndef NDEBUG
      checker::SanityCheckItem(&k, &v);
#endif
      const size_t sz = Size(v);
      total_sz += sz;
      TryVerifyStrict(tbl_item(1)->InsertRecord(
          txn, Encode(str(Size(k)), k),
          Encode(str(sz), v)));  // this table is shared, so any partition is OK
      TryVerifyStrict(db->Commit(txn));
    }
    if (ermia::config::verbose) {
      std::cerr << "[INFO] finished loading item" << std::endl;
      std::cerr << "[INFO]   * average item record length: "
           << (double(total_sz) / double(NumItems())) << " bytes" << std::endl;
    }
  }
};

class tpcc_stock_loader : public bench_loader, public tpcc_worker_mixin {
 public:
  tpcc_stock_loader(unsigned long seed, ermia::Engine *db,
                    const std::map<std::string, ermia::OrderedIndex *> &open_tables,
                    const std::map<std::string, std::vector<ermia::OrderedIndex *>> &partitions,
                    ssize_t warehouse_id)
      : bench_loader(seed, db, open_tables),
        tpcc_worker_mixin(partitions),
        warehouse_id(warehouse_id) {
    ALWAYS_ASSERT(warehouse_id == -1 ||
                  (warehouse_id >= 1 &&
                   static_cast<size_t>(warehouse_id) <= NumWarehouses()));
  }

 protected:
  virtual void load() {
    std::string obj_buf, obj_buf1;

    uint64_t stock_total_sz = 0, n_stocks = 0;
    const uint w_start =
        (warehouse_id == -1) ? 1 : static_cast<uint>(warehouse_id);
    const uint w_end = (warehouse_id == -1) ? NumWarehouses()
                                            : static_cast<uint>(warehouse_id);

    for (uint w = w_start; w <= w_end; w++) {
      const size_t batchsize = 10;
      for (size_t i = 0; i < NumItems();) {
        size_t iend = std::min(i + batchsize, NumItems());
        ermia::scoped_str_arena s_arena(*arena);
        for (uint j = i + 1; j <= iend; j++) {
          arena->reset();
          ermia::transaction *const txn = db->NewTransaction(0, *arena, txn_buf());
          const stock::key k(w, j);
          const stock_data::key k_data(w, j);

          stock::value v;
          v.s_quantity = RandomNumber(r, 10, 100);
          v.s_ytd = 0;
          v.s_order_cnt = 0;
          v.s_remote_cnt = 0;

          stock_data::value v_data;
          const int len = RandomNumber(r, 26, 50);
          if (RandomNumber(r, 1, 100) > 10) {
            const std::string s_data = RandomStr(r, len);
            v_data.s_data.assign(s_data);
          } else {
            const int startOriginal = RandomNumber(r, 2, (len - 8));
            const std::string s_data = RandomStr(r, startOriginal + 1) + "ORIGINAL" +
                                  RandomStr(r, len - startOriginal - 7);
            v_data.s_data.assign(s_data);
          }
          v_data.s_dist_01.assign(RandomStr(r, 24));
          v_data.s_dist_02.assign(RandomStr(r, 24));
          v_data.s_dist_03.assign(RandomStr(r, 24));
          v_data.s_dist_04.assign(RandomStr(r, 24));
          v_data.s_dist_05.assign(RandomStr(r, 24));
          v_data.s_dist_06.assign(RandomStr(r, 24));
          v_data.s_dist_07.assign(RandomStr(r, 24));
          v_data.s_dist_08.assign(RandomStr(r, 24));
          v_data.s_dist_09.assign(RandomStr(r, 24));
          v_data.s_dist_10.assign(RandomStr(r, 24));

#ifndef NDEBUG
          checker::SanityCheckStock(&k);
#endif
          const size_t sz = Size(v);
          stock_total_sz += sz;
          n_stocks++;
          TryVerifyStrict(tbl_stock(w)->InsertRecord(txn, Encode(str(Size(k)), k),
                                                 Encode(str(sz), v)));
          TryVerifyStrict(
              tbl_stock_data(w)->InsertRecord(txn, Encode(str(Size(k_data)), k_data),
                                        Encode(str(Size(v_data)), v_data)));
          TryVerifyStrict(db->Commit(txn));
        }

        // loop update
        i = iend;
      }
    }
    if (ermia::config::verbose) {
      if (warehouse_id == -1) {
        std::cerr << "[INFO] finished loading stock" << std::endl;
        std::cerr << "[INFO]   * average stock record length: "
             << (double(stock_total_sz) / double(n_stocks)) << " bytes" << std::endl;
      } else {
        std::cerr << "[INFO] finished loading stock (w=" << warehouse_id << ")"
             << std::endl;
      }
    }
  }

 private:
  ssize_t warehouse_id;
};

class tpcc_district_loader : public bench_loader, public tpcc_worker_mixin {
 public:
  tpcc_district_loader(unsigned long seed, ermia::Engine *db,
                       const std::map<std::string, ermia::OrderedIndex *> &open_tables,
                       const std::map<std::string, std::vector<ermia::OrderedIndex *>> &partitions)
      : bench_loader(seed, db, open_tables), tpcc_worker_mixin(partitions) {}

 protected:
  virtual void load() {
    std::string obj_buf;

    const ssize_t bsize = 10;
    uint64_t district_total_sz = 0, n_districts = 0;
    uint cnt = 0;
    for (uint w = 1; w <= NumWarehouses(); w++) {
      for (uint d = 1; d <= NumDistrictsPerWarehouse(); d++, cnt++) {
        arena->reset();
        ermia::transaction *txn = db->NewTransaction(0, *arena, txn_buf());
        const district::key k(w, d);

        district::value v;
        v.d_ytd = 30000;
        v.d_tax = (float)(RandomNumber(r, 0, 2000) / 10000.0);
        v.d_next_o_id = 3001;
        v.d_name.assign(RandomStr(r, RandomNumber(r, 6, 10)));
        v.d_street_1.assign(RandomStr(r, RandomNumber(r, 10, 20)));
        v.d_street_2.assign(RandomStr(r, RandomNumber(r, 10, 20)));
        v.d_city.assign(RandomStr(r, RandomNumber(r, 10, 20)));
        v.d_state.assign(RandomStr(r, 3));
        v.d_zip.assign("123456789");

#ifndef NDEBUG
        checker::SanityCheckDistrict(&k, &v);
#endif
        const size_t sz = Size(v);
        district_total_sz += sz;
        n_districts++;
        TryVerifyStrict(tbl_district(w)->InsertRecord(txn, Encode(str(Size(k)), k),
                                                  Encode(str(sz), v)));

        TryVerifyStrict(db->Commit(txn));
      }
    }
    if (ermia::config::verbose) {
      std::cerr << "[INFO] finished loading district" << std::endl;
      std::cerr << "[INFO]   * average district record length: "
           << (double(district_total_sz) / double(n_districts)) << " bytes"
           << std::endl;
    }
  }
};

class tpcc_customer_loader : public bench_loader, public tpcc_worker_mixin {
 public:
  tpcc_customer_loader(unsigned long seed, ermia::Engine *db,
                       const std::map<std::string, ermia::OrderedIndex *> &open_tables,
                       const std::map<std::string, std::vector<ermia::OrderedIndex *>> &partitions,
                       ssize_t warehouse_id)
      : bench_loader(seed, db, open_tables),
        tpcc_worker_mixin(partitions),
        warehouse_id(warehouse_id) {
    ALWAYS_ASSERT(warehouse_id == -1 ||
                  (warehouse_id >= 1 &&
                   static_cast<size_t>(warehouse_id) <= NumWarehouses()));
  }

 protected:
  virtual void load() {
    std::string obj_buf;

    const uint w_start =
        (warehouse_id == -1) ? 1 : static_cast<uint>(warehouse_id);
    const uint w_end = (warehouse_id == -1) ? NumWarehouses()
                                            : static_cast<uint>(warehouse_id);
    const size_t batchsize = 10;
    const size_t nbatches = (batchsize > NumCustomersPerDistrict())
                                ? 1
                                : (NumCustomersPerDistrict() / batchsize);

    uint64_t total_sz = 0;

    for (uint w = w_start; w <= w_end; w++) {
      for (uint d = 1; d <= NumDistrictsPerWarehouse(); d++) {
        for (uint batch = 0; batch < nbatches;) {
          const size_t cstart = batch * batchsize;
          const size_t cend =
              std::min((batch + 1) * batchsize, NumCustomersPerDistrict());
          for (uint cidx0 = cstart; cidx0 < cend; cidx0++) {
            ermia::scoped_str_arena s_arena(arena);
            arena->reset();
            ermia::transaction *txn = db->NewTransaction(0, *arena, txn_buf());
            const uint c = cidx0 + 1;
            const customer::key k(w, d, c);

            customer::value v;
            v.c_id = c;  // Put the c_id here in the tuple, needed by
                         // order-status later
            v.c_discount = (float)(RandomNumber(r, 1, 5000) / 10000.0);
            if (RandomNumber(r, 1, 100) <= 10)
              v.c_credit.assign("BC");
            else
              v.c_credit.assign("GC");

            if (c <= 1000)
              v.c_last.assign(GetCustomerLastName(c - 1));
            else
              v.c_last.assign(GetNonUniformCustomerLastNameLoad(r));

            v.c_first.assign(RandomStr(r, RandomNumber(r, 8, 16)));
            v.c_credit_lim = 50000;

            v.c_balance = -10;
            v.c_ytd_payment = 10;
            v.c_payment_cnt = 1;
            v.c_delivery_cnt = 0;

            v.c_street_1.assign(RandomStr(r, RandomNumber(r, 10, 20)));
            v.c_street_2.assign(RandomStr(r, RandomNumber(r, 10, 20)));
            v.c_city.assign(RandomStr(r, RandomNumber(r, 10, 20)));
            v.c_state.assign(RandomStr(r, 3));
            v.c_zip.assign(RandomNStr(r, 4) + "11111");
            v.c_phone.assign(RandomNStr(r, 16));
            v.c_since = GetCurrentTimeMillis();
            v.c_middle.assign("OE");
            v.c_data.assign(RandomStr(r, RandomNumber(r, 300, 500)));

#ifndef NDEBUG
            checker::SanityCheckCustomer(&k, &v);
#endif
            const size_t sz = Size(v);
            total_sz += sz;
            ermia::OID c_oid = 0;  // Get the OID and put in customer_name_idx later
            TryVerifyStrict(tbl_customer(w)->InsertRecord(
                txn, Encode(str(Size(k)), k), Encode(str(sz), v), &c_oid));
            TryVerifyStrict(db->Commit(txn));

            // customer name index
            const customer_name_idx::key k_idx(
                k.c_w_id, k.c_d_id, v.c_last.str(true), v.c_first.str(true));

            // index structure is:
            // (c_w_id, c_d_id, c_last, c_first) -> OID

            arena->reset();
            txn = db->NewTransaction(0, *arena, txn_buf());
            TryVerifyStrict(tbl_customer_name_idx(w)->InsertOID(
                txn, Encode(str(Size(k_idx)), k_idx), c_oid));
            TryVerifyStrict(db->Commit(txn));
            arena->reset();

            history::key k_hist;
            k_hist.h_c_id = c;
            k_hist.h_c_d_id = d;
            k_hist.h_c_w_id = w;
            k_hist.h_d_id = d;
            k_hist.h_w_id = w;
            k_hist.h_date = GetCurrentTimeMillis();

            history::value v_hist;
            v_hist.h_amount = 10;
            v_hist.h_data.assign(RandomStr(r, RandomNumber(r, 10, 24)));

            arena->reset();
            txn = db->NewTransaction(0, *arena, txn_buf());
            TryVerifyStrict(
                tbl_history(w)->InsertRecord(txn, Encode(str(Size(k_hist)), k_hist),
                                       Encode(str(Size(v_hist)), v_hist)));
            TryVerifyStrict(db->Commit(txn));
          }
          batch++;
        }
      }
    }
    if (ermia::config::verbose) {
      if (warehouse_id == -1) {
        std::cerr << "[INFO] finished loading customer" << std::endl;
        std::cerr << "[INFO]   * average customer record length: "
             << (double(total_sz) /
                 double(NumWarehouses() * NumDistrictsPerWarehouse() *
                        NumCustomersPerDistrict())) << " bytes " << std::endl;
      } else {
        std::cerr << "[INFO] finished loading customer (w=" << warehouse_id << ")"
             << std::endl;
      }
    }
  }

 private:
  ssize_t warehouse_id;
};

class tpcc_order_loader : public bench_loader, public tpcc_worker_mixin {
 public:
  tpcc_order_loader(unsigned long seed, ermia::Engine *db,
                    const std::map<std::string, ermia::OrderedIndex *> &open_tables,
                    const std::map<std::string, std::vector<ermia::OrderedIndex *>> &partitions,
                    ssize_t warehouse_id)
      : bench_loader(seed, db, open_tables),
        tpcc_worker_mixin(partitions),
        warehouse_id(warehouse_id) {
    ALWAYS_ASSERT(warehouse_id == -1 ||
                  (warehouse_id >= 1 &&
                   static_cast<size_t>(warehouse_id) <= NumWarehouses()));
  }

 protected:
  size_t NumOrderLinesPerCustomer() { return RandomNumber(r, 5, 15); }

  virtual void load() {
    std::string obj_buf;

    uint64_t order_line_total_sz = 0, n_order_lines = 0;
    uint64_t oorder_total_sz = 0, n_oorders = 0;
    uint64_t new_order_total_sz = 0, n_new_orders = 0;

    const uint w_start =
        (warehouse_id == -1) ? 1 : static_cast<uint>(warehouse_id);
    const uint w_end = (warehouse_id == -1) ? NumWarehouses()
                                            : static_cast<uint>(warehouse_id);

    for (uint w = w_start; w <= w_end; w++) {
      for (uint d = 1; d <= NumDistrictsPerWarehouse(); d++) {
        std::set<uint> c_ids_s;
        std::vector<uint> c_ids;
        while (c_ids.size() != NumCustomersPerDistrict()) {
          const auto x = (r.next() % NumCustomersPerDistrict()) + 1;
          if (c_ids_s.count(x)) continue;
          c_ids_s.insert(x);
          c_ids.emplace_back(x);
        }
        for (uint c = 1; c <= NumCustomersPerDistrict();) {
          ermia::scoped_str_arena s_arena(arena);
          arena->reset();
          ermia::transaction *txn = db->NewTransaction(0, *arena, txn_buf());
          const oorder::key k_oo(w, d, c);

          oorder::value v_oo;
          v_oo.o_c_id = c_ids[c - 1];
          if (k_oo.o_id < 2101)
            v_oo.o_carrier_id = RandomNumber(r, 1, 10);
          else
            v_oo.o_carrier_id = 0;
          v_oo.o_ol_cnt = NumOrderLinesPerCustomer();
          v_oo.o_all_local = 1;
          v_oo.o_entry_d = GetCurrentTimeMillis();

#ifndef NDEBUG
          checker::SanityCheckOOrder(&k_oo, &v_oo);
#endif
          const size_t sz = Size(v_oo);
          oorder_total_sz += sz;
          n_oorders++;
          ermia::OID v_oo_oid = 0;  // Get the OID and put it in oorder_c_id_idx later
          TryVerifyStrict(
              tbl_oorder(w)->InsertRecord(txn, Encode(str(Size(k_oo)), k_oo),
                                    Encode(str(sz), v_oo), &v_oo_oid));
          TryVerifyStrict(db->Commit(txn));
          arena->reset();
          txn = db->NewTransaction(0, *arena, txn_buf());

          const oorder_c_id_idx::key k_oo_idx(k_oo.o_w_id, k_oo.o_d_id,
                                              v_oo.o_c_id, k_oo.o_id);
          TryVerifyStrict(tbl_oorder_c_id_idx(w)->InsertOID(
              txn, Encode(str(Size(k_oo_idx)), k_oo_idx), v_oo_oid));
          TryVerifyStrict(db->Commit(txn));

          if (c >= 2101) {
            arena->reset();
            txn = db->NewTransaction(0, *arena, txn_buf());
            const new_order::key k_no(w, d, c);
            const new_order::value v_no;

#ifndef NDEBUG
            checker::SanityCheckNewOrder(&k_no);
#endif
            const size_t sz = Size(v_no);
            new_order_total_sz += sz;
            n_new_orders++;
            TryVerifyStrict(tbl_new_order(w)->InsertRecord(
                txn, Encode(str(Size(k_no)), k_no), Encode(str(sz), v_no)));
            TryVerifyStrict(db->Commit(txn));
          }

          for (uint l = 1; l <= uint(v_oo.o_ol_cnt); l++) {
            const order_line::key k_ol(w, d, c, l);

            order_line::value v_ol;
            v_ol.ol_i_id = RandomNumber(r, 1, 100000);
            if (k_ol.ol_o_id < 2101) {
              v_ol.ol_delivery_d = v_oo.o_entry_d;
              v_ol.ol_amount = 0;
            } else {
              v_ol.ol_delivery_d = 0;
              // random within [0.01 .. 9,999.99]
              v_ol.ol_amount = (float)(RandomNumber(r, 1, 999999) / 100.0);
            }

            v_ol.ol_supply_w_id = k_ol.ol_w_id;
            v_ol.ol_quantity = 5;
            // v_ol.ol_dist_info comes from stock_data(ol_supply_w_id, ol_o_id)
            // v_ol.ol_dist_info = RandomStr(r, 24);

#ifndef NDEBUG
            checker::SanityCheckOrderLine(&k_ol, &v_ol);
#endif
            const size_t sz = Size(v_ol);
            order_line_total_sz += sz;
            n_order_lines++;
            arena->reset();
            txn = db->NewTransaction(0, *arena, txn_buf());
            TryVerifyStrict(tbl_order_line(w)->InsertRecord(
                txn, Encode(str(Size(k_ol)), k_ol), Encode(str(sz), v_ol)));
            TryVerifyStrict(db->Commit(txn));
          }
          c++;
        }
      }
    }

    if (ermia::config::verbose) {
      if (warehouse_id == -1) {
        std::cerr << "[INFO] finished loading order" << std::endl;
        std::cerr << "[INFO]   * average order_line record length: "
             << (double(order_line_total_sz) / double(n_order_lines))
             << " bytes" << std::endl;
        std::cerr << "[INFO]   * average oorder record length: "
             << (double(oorder_total_sz) / double(n_oorders)) << " bytes"
             << std::endl;
        std::cerr << "[INFO]   * average new_order record length: "
             << (double(new_order_total_sz) / double(n_new_orders)) << " bytes"
             << std::endl;
      } else {
        std::cerr << "[INFO] finished loading order (w=" << warehouse_id << ")"
             << std::endl;
      }
    }
  }

 private:
  ssize_t warehouse_id;
};