#pragma once

#include "controller/kalman/kalman.hpp"
#include "core/component.hpp"
#include "core/component_registry.hpp"
#include <ryml/ryml.hpp>

namespace nuedcs::controller::kalman {

template <typename Derived>
inline bool load_matrix(ryml::NodeRef node, const char* name, Eigen::MatrixBase<Derived>& out) {
    constexpr int Rows = Derived::RowsAtCompileTime;
    constexpr int Cols = Derived::ColsAtCompileTime;
    constexpr int N    = Rows * Cols;

    if (!node.has_child(c4::to_csubstr(name))) return false;
    auto child = node[name];
    if (!child.is_seq() || child.num_children() < N) return false;

    double val[N];
    int idx = 0;
    for (auto sub : child.children()) {
        if (idx < N) sub >> val[idx++];
    }
    if (idx != N) return false;

    // Eigen is column-major; YAML is row-major → transpose after fill
    Eigen::Matrix<double, Rows, Cols> tmp;
    for (int i = 0; i < N; ++i)
        tmp(i) = val[i];
    const_cast<Eigen::MatrixBase<Derived>&>(out) = tmp.transpose();
    return true;
}

template <typename Derived, typename DefaultExpr>
inline void load_matrix_or(ryml::NodeRef node, const char* name, Eigen::MatrixBase<Derived>& out,
    const Eigen::MatrixBase<DefaultExpr>& default_val) {
    if (!load_matrix(node, name, out))
        const_cast<Eigen::MatrixBase<Derived>&>(out) = default_val;
}

template <int N, int M = N, int L = N>
class KalmanComponent : public core::Component {
public:
    using KF           = KalmanFilter<N, M, L>;
    using StateVec     = typename KF::StateVec;
    using MeasVec      = typename KF::MeasVec;
    using ControlVec   = typename KF::ControlVec;
    using StateMat     = typename KF::StateMat;
    using MeasMat      = typename KF::MeasMat;
    using MeasStateMat = typename KF::MeasStateMat;
    using StateCtrlMat = typename KF::StateCtrlMat;

    explicit KalmanComponent(ryml::NodeRef config) {
        auto c = core::Config { config };

        // Load system matrices from YAML (all have sane defaults)
        StateMat A, Q;
        MeasMat R;
        MeasStateMat H;
        StateCtrlMat B;
        StateVec x0;
        StateMat P0;

        load_matrix_or(config, "A", A, StateMat::Identity());
        load_matrix_or(config, "Q", Q, StateMat::Identity());
        load_matrix_or(config, "R", R, MeasMat::Identity());
        load_matrix_or(config, "H", H, MeasStateMat::Identity());
        load_matrix_or(config, "B", B, StateCtrlMat::Zero());
        load_matrix_or(config, "x0", x0, StateVec::Zero());
        load_matrix_or(config, "P0", P0, StateMat::Identity());

        kf_.set_A(A);
        kf_.set_Q(Q);
        kf_.set_R(R);
        kf_.set_H(H);
        kf_.set_B(B);
        kf_.set_state(x0);
        kf_.set_covariance(P0);

        // I/O ports
        register_input(c["measurement"].str(), z_in_);
        register_input(c["control"].str(), u_in_, false);
        register_output(c["state"].str(), state_out_, kf_.state());
        register_output(c["covariance"].str(), cov_out_, kf_.covariance());
    }

    void update() override {
        kf_.predict(*u_in_);
        kf_.update(*z_in_);
        *state_out_ = kf_.state();
        *cov_out_   = kf_.covariance();
    }

    // ── Direct filter access ──────────────────────────────────────
    [[nodiscard]] KF& filter() { return kf_; }
    [[nodiscard]] const KF& filter() const { return kf_; }

private:
    KF kf_;

    InputInterface<MeasVec> z_in_;
    InputInterface<ControlVec> u_in_;

    OutputInterface<StateVec> state_out_;
    OutputInterface<StateMat> cov_out_;
};

// ── Convenience aliases ───────────────────────────────────────────

using KalmanComponent2d = KalmanComponent<2>;

} // namespace nuedcs::controller::kalman

REGISTER_COMPONENT(nuedcs::controller::kalman, KalmanComponent2d)
