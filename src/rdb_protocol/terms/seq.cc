// Copyright 2010-2013 RethinkDB, all rights reserved.
#include "rdb_protocol/terms/terms.hpp"

#include <string>
#include <utility>
#include <vector>

#include "rdb_protocol/changefeed.hpp"
#include "rdb_protocol/error.hpp"
#include "rdb_protocol/func.hpp"
#include "rdb_protocol/op.hpp"

namespace ql {

// RSI: Double-check op_is_deterministic impls in this file.

template<class T>
class map_acc_term_t : public grouped_seq_op_term_t {
protected:
    map_acc_term_t(compile_env_t *env, const protob_t<const Term> &term)
        : grouped_seq_op_term_t(env, term, argspec_t(1, 2)) { }
private:
    counted_t<val_t> eval_impl(scope_env_t *env, args_t *args,
                               eval_flags_t) const FINAL {
        return args->num_args() == 1
            ? args->arg(env, 0)->as_seq(env->env)->run_terminal(env->env, T(backtrace()))
            : args->arg(env, 0)->as_seq(env->env)->run_terminal(
                env->env, T(backtrace(), args->arg(env, 1)->as_func(GET_FIELD_SHORTCUT)));
    }

    // RSI: Yeah, this'll need to change, once we parallelize terminals.  The
    // function (arg 1, if it exists) could have a non-zero parallelization level.
    int parallelization_level() const FINAL {
        return params_parallelization_level();
    }
};

class sum_term_t : public map_acc_term_t<sum_wire_func_t> {
public:
    template<class... Args> sum_term_t(Args... args)
        : map_acc_term_t<sum_wire_func_t>(args...) { }
private:
    const char *name() const FINAL { return "sum"; }

    bool op_is_deterministic() const { return true; }
};
class avg_term_t : public map_acc_term_t<avg_wire_func_t> {
public:
    template<class... Args> avg_term_t(Args... args)
        : map_acc_term_t<avg_wire_func_t>(args...) { }
private:
    const char *name() const FINAL { return "avg"; }

    bool op_is_deterministic() const { return true; }
};
class min_term_t : public map_acc_term_t<min_wire_func_t> {
public:
    template<class... Args> min_term_t(Args... args)
        : map_acc_term_t<min_wire_func_t>(args...) { }
private:
    const char *name() const FINAL { return "min"; }

    bool op_is_deterministic() const { return true; }
};
class max_term_t : public map_acc_term_t<max_wire_func_t> {
public:
    template<class... Args> max_term_t(Args... args)
        : map_acc_term_t<max_wire_func_t>(args...) { }
private:
    const char *name() const FINAL { return "max"; }

    bool op_is_deterministic() const { return true; }
};

class count_term_t : public grouped_seq_op_term_t {
public:
    count_term_t(compile_env_t *env, const protob_t<const Term> &term)
        : grouped_seq_op_term_t(env, term, argspec_t(1, 2)) { }
private:
    counted_t<val_t> eval_impl(scope_env_t *env, args_t *args,
                               eval_flags_t) const FINAL {
        counted_t<val_t> v0 = args->arg(env, 0);
        if (args->num_args() == 1) {
            return v0->as_seq(env->env)
                ->run_terminal(env->env, count_wire_func_t());
        } else {
            counted_t<val_t> v1 = args->arg(env, 1);
            if (v1->get_type().is_convertible(val_t::type_t::FUNC)) {
                counted_t<datum_stream_t> stream = v0->as_seq(env->env);
                stream->add_transformation(
                        filter_wire_func_t(v1->as_func(), boost::none),
                        backtrace());
                return stream->run_terminal(env->env, count_wire_func_t());
            } else {
                counted_t<func_t> f =
                    new_eq_comparison_func(v1->as_datum(), backtrace());
                counted_t<datum_stream_t> stream = v0->as_seq(env->env);
                stream->add_transformation(
                        filter_wire_func_t(f, boost::none), backtrace());

                return stream->run_terminal(env->env, count_wire_func_t());
            }
        }
    }
    const char *name() const FINAL { return "count"; }

    // A count of a stream has the same parallelizability as returning its
    // rows... though it might be cheaper, someday.
    int parallelization_level() const FINAL {
        return params_parallelization_level();
    }

    bool op_is_deterministic() const FINAL { return true; }
};

class map_term_t : public grouped_seq_op_term_t {
public:
    map_term_t(compile_env_t *env, const protob_t<const Term> &term)
        : grouped_seq_op_term_t(env, term, argspec_t(2)) { }
private:
    counted_t<val_t> eval_impl(scope_env_t *env, args_t *args, eval_flags_t) const FINAL {
        counted_t<datum_stream_t> stream = args->arg(env, 0)->as_seq(env->env);
        stream->add_transformation(
                map_wire_func_t(args->arg(env, 1)->as_func()), backtrace());
        return new_val(env->env, stream);
    }
    const char *name() const FINAL { return "map"; }

    bool op_is_deterministic() const FINAL { return true; }

    // RSI: This'll need to change once we parallelize transformations.
    int parallelization_level() const FINAL {
        return params_parallelization_level();
    }
};

class concatmap_term_t : public grouped_seq_op_term_t {
public:
    concatmap_term_t(compile_env_t *env, const protob_t<const Term> &term)
        : grouped_seq_op_term_t(env, term, argspec_t(2)) { }
private:
    counted_t<val_t> eval_impl(scope_env_t *env, args_t *args, eval_flags_t) const FINAL {
        counted_t<datum_stream_t> stream = args->arg(env, 0)->as_seq(env->env);
        stream->add_transformation(
                concatmap_wire_func_t(args->arg(env, 1)->as_func()), backtrace());
        return new_val(env->env, stream);
    }
    const char *name() const FINAL { return "concatmap"; }

    bool op_is_deterministic() const FINAL { return true; }

    // RSI: This'll need to change once we parallelize transformations.
    int parallelization_level() const FINAL {
        return params_parallelization_level();
    }
};

class group_term_t : public grouped_seq_op_term_t {
public:
    group_term_t(compile_env_t *env, const protob_t<const Term> &term)
        : grouped_seq_op_term_t(env, term, argspec_t(1, -1), optargspec_t({"index", "multi"})) { }
private:
    counted_t<val_t> eval_impl(scope_env_t *env, args_t *args, eval_flags_t) const FINAL {
        std::vector<counted_t<func_t> > funcs;
        funcs.reserve(args->num_args() - 1);
        for (size_t i = 1; i < args->num_args(); ++i) {
            funcs.push_back(args->arg(env, i)->as_func(GET_FIELD_SHORTCUT));
        }

        counted_t<datum_stream_t> seq;
        bool append_index = false;
        if (counted_t<val_t> index = args->optarg(env, "index")) {
            std::string index_str = index->as_str().to_std();
            counted_t<table_t> tbl = args->arg(env, 0)->as_table();
            if (index_str == tbl->get_pkey()) {
                auto field = make_counted<const datum_t>(std::move(index_str));
                funcs.push_back(new_get_field_func(field, backtrace()));
            } else {
                tbl->add_sorting(index_str, sorting_t::ASCENDING, this);
                append_index = true;
            }
            seq = tbl->as_datum_stream(env->env, backtrace());
        } else {
            seq = args->arg(env, 0)->as_seq(env->env);
        }

        rcheck((funcs.size() + append_index) != 0, base_exc_t::GENERIC,
               "Cannot group by nothing.");

        bool multi = false;
        if (counted_t<val_t> multi_val = args->optarg(env, "multi")) {
            multi = multi_val->as_bool();
        }

        seq->add_grouping(group_wire_func_t(std::move(funcs),
                                            append_index,
                                            multi),
                          backtrace());

        return new_val(env->env, seq);
    }
    const char *name() const FINAL { return "group"; }

    // RSI: On arrays, will a group operation preserve ordering?
    bool op_is_deterministic() const FINAL { return true; }

    // RSI: This'll need to change once we parallelize transformations?  How exactly
    // does grouping affect life?
    int parallelization_level() const FINAL {
        return params_parallelization_level();
    }
};

class filter_term_t : public grouped_seq_op_term_t {
public:
    filter_term_t(compile_env_t *env, const protob_t<const Term> &term)
        : grouped_seq_op_term_t(env, term, argspec_t(2), optargspec_t({"default"})),
          default_filter_term(lazy_literal_optarg(env, "default")) { }

private:
    counted_t<val_t> eval_impl(scope_env_t *env, args_t *args, eval_flags_t) const FINAL {
        counted_t<val_t> v0 = args->arg(env, 0);
        counted_t<val_t> v1 = args->arg(env, 1, LITERAL_OK);
        counted_t<func_t> f = v1->as_func(CONSTANT_SHORTCUT);
        boost::optional<wire_func_t> defval;
        if (default_filter_term.has()) {
            defval = wire_func_t(default_filter_term->eval_to_func(env->scope));
        }

        if (v0->get_type().is_convertible(val_t::type_t::SELECTION)) {
            std::pair<counted_t<table_t>, counted_t<datum_stream_t> > ts
                = v0->as_selection(env->env);
            ts.second->add_transformation(
                    filter_wire_func_t(f, defval), backtrace());
            return new_val(ts.second, ts.first);
        } else {
            counted_t<datum_stream_t> stream = v0->as_seq(env->env);
            stream->add_transformation(
                    filter_wire_func_t(f, defval), backtrace());
            return new_val(env->env, stream);
        }
    }

    const char *name() const FINAL { return "filter"; }

    bool op_is_deterministic() const FINAL { return true; }

    // RSI: This'll need to change once we parallelize transformations.
    int parallelization_level() const FINAL {
        return params_parallelization_level();
    }

    counted_t<func_term_t> default_filter_term;
};

class reduce_term_t : public grouped_seq_op_term_t {
public:
    reduce_term_t(compile_env_t *env, const protob_t<const Term> &term) :
        grouped_seq_op_term_t(env, term, argspec_t(2), optargspec_t({ "base" })) { }
private:
    counted_t<val_t> eval_impl(scope_env_t *env, args_t *args, eval_flags_t) const FINAL {
        return args->arg(env, 0)->as_seq(env->env)->run_terminal(
            env->env, reduce_wire_func_t(args->arg(env, 1)->as_func()));
    }
    const char *name() const FINAL { return "reduce"; }

    bool op_is_deterministic() const FINAL { return true; }

    // RSI: This'll need to change once we parallelize transformations/terminals.
    int parallelization_level() const FINAL {
        return params_parallelization_level();
    }
};

class changes_term_t : public op_term_t {
public:
    changes_term_t(compile_env_t *env, const protob_t<const Term> &term)
        : op_term_t(env, term, argspec_t(1)) { }
private:
    counted_t<val_t> eval_impl(scope_env_t *env, args_t *args, eval_flags_t) const FINAL {
        counted_t<table_t> tbl = args->arg(env, 0)->as_table();
        changefeed::client_t *client = env->env->get_changefeed_client();
        return new_val(env->env, client->new_feed(tbl, env->env));
    }
    const char *name() const FINAL { return "changes"; }

    // We should never be asking if the operation is deterministic... but I think we
    // might.  Anyway, it isn't.
    bool op_is_deterministic() const { return false; }

    // RSI: Um.  Maybe the API should be changed because with some expressions,
    // parallelizing them is a bit nonsensical.
    int parallelization_level() const FINAL {
        return params_parallelization_level();
    }
};

// TODO: this sucks.  Change to use the same macros as rewrites.hpp?
class between_term_t : public bounded_op_term_t {
public:
    between_term_t(compile_env_t *env, const protob_t<const Term> &term)
        : bounded_op_term_t(env, term, argspec_t(3), optargspec_t({"index"})) { }
private:
    counted_t<val_t> eval_impl(scope_env_t *env, args_t *args, eval_flags_t) const FINAL {
        counted_t<table_t> tbl = args->arg(env, 0)->as_table();
        bool left_open = is_left_open(env, args);
        counted_t<const datum_t> lb = args->arg(env, 1)->as_datum();
        if (lb->get_type() == datum_t::R_NULL) {
            lb.reset();
        }
        bool right_open = is_right_open(env, args);
        counted_t<const datum_t> rb = args->arg(env, 2)->as_datum();
        if (rb->get_type() == datum_t::R_NULL) {
            rb.reset();
        }

        if (lb.has() && rb.has()) {
            if (*lb > *rb || ((left_open || right_open) && *lb == *rb)) {
                counted_t<const datum_t> arr = make_counted<datum_t>(datum_t::R_ARRAY);
                counted_t<datum_stream_t> ds(
                    new array_datum_stream_t(arr, backtrace()));
                return new_val(ds, tbl);
            }
        }

        counted_t<val_t> sindex = args->optarg(env, "index");
        std::string sid = (sindex.has() ? sindex->as_str().to_std() : tbl->get_pkey());

        tbl->add_bounds(
            datum_range_t(
                lb, left_open ? key_range_t::open : key_range_t::closed,
                rb, right_open ? key_range_t::open : key_range_t::closed),
            sid, this);
        return new_val(tbl);
    }
    const char *name() const FINAL { return "between"; }

    // RSI: filter_func is unused?
    protob_t<Term> filter_func;

    // Apparently this can only be called on a table.  Welp, we're deterministic if
    // the table is.
    bool op_is_deterministic() const { return true; }

    // A .between on a stream or anything doesn't change the parallelizability of the
    // operation.
    int parallelization_level() const FINAL {
        return params_parallelization_level();
    }
};

class union_term_t : public op_term_t {
public:
    union_term_t(compile_env_t *env, const protob_t<const Term> &term)
        : op_term_t(env, term, argspec_t(0, -1)) { }
private:
    counted_t<val_t> eval_impl(scope_env_t *env, args_t *args, eval_flags_t) const FINAL {
        std::vector<counted_t<datum_stream_t> > streams;
        for (size_t i = 0; i < args->num_args(); ++i) {
            streams.push_back(args->arg(env, i)->as_seq(env->env));
        }
        counted_t<datum_stream_t> union_stream
            = make_counted<union_datum_stream_t>(std::move(streams), backtrace());
        return new_val(env->env, union_stream);
    }
    const char *name() const FINAL { return "union"; }

    // We don't promise a particular ordering when combining two other streams.
    // RSI: Maybe with arrays... we do and should return true for this?
    bool op_is_deterministic() const FINAL { return false; }

    // RSI: Once we parallelize union_datum_stream_t, this'll need to change.
    int parallelization_level() const FINAL {
        return params_parallelization_level();
    }
};

class zip_term_t : public op_term_t {
public:
    zip_term_t(compile_env_t *env, const protob_t<const Term> &term)
        : op_term_t(env, term, argspec_t(1)) { }
private:
    counted_t<val_t> eval_impl(scope_env_t *env, args_t *args, eval_flags_t) const FINAL {
        return new_val(env->env, args->arg(env, 0)->as_seq(env->env)->zip());
    }
    const char *name() const FINAL { return "zip"; }

    // This just maps a deterministic function (merging left and right fields), so
    // it's deterministic if the stream it's called on is.
    bool op_is_deterministic() const FINAL { return true; }

    // This maps a non-blocking operation on a stream, so its parallelizability is
    // the same as that of its parameter.
    int parallelization_level() const FINAL {
        return params_parallelization_level();
    }
};

counted_t<term_t> make_between_term(
    compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<between_term_t>(env, term);
}
counted_t<term_t> make_changes_term(
    compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<changes_term_t>(env, term);
}
counted_t<term_t> make_reduce_term(
    compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<reduce_term_t>(env, term);
}
counted_t<term_t> make_map_term(
    compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<map_term_t>(env, term);
}
counted_t<term_t> make_filter_term(
    compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<filter_term_t>(env, term);
}
counted_t<term_t> make_concatmap_term(
    compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<concatmap_term_t>(env, term);
}
counted_t<term_t> make_group_term(
    compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<group_term_t>(env, term);
}
counted_t<term_t> make_count_term(
    compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<count_term_t>(env, term);
}
counted_t<term_t> make_avg_term(
    compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<avg_term_t>(env, term);
}
counted_t<term_t> make_sum_term(
    compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<sum_term_t>(env, term);
}
counted_t<term_t> make_min_term(
    compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<min_term_t>(env, term);
}
counted_t<term_t> make_max_term(
    compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<max_term_t>(env, term);
}
counted_t<term_t> make_union_term(
    compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<union_term_t>(env, term);
}
counted_t<term_t> make_zip_term(
    compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<zip_term_t>(env, term);
}

} // namespace ql
