#pragma once

#include "core/component.hpp"
#include "core/component_registry.hpp"
#include <Eigen/Dense>
#include <ryml/ryml.hpp>
#include <vector>

namespace nuedcs::controller::kalman {

template <int Rows, int Cols>
inline Eigen::Matrix<double, Rows, Cols> load_matrix(ryml::NodeRef node, const char* name, const Eigen::Matrix<double, Rows, Cols>& default_val) {
    if (node.has_child(c4::to_csubstr(name))) {
        auto child = node[name];
        constexpr int expected_elements = Rows * Cols;
        if (child.is_seq() && child.num_children() >= expected_elements) {
            std::vector<double> val(expected_elements);
            int idx = 0;
            for (auto sub_node : child.children()) {
                if (idx < expected_elements) {
                    sub_node >> val[idx++];
                }
            }
            if (idx == expected_elements) {
                Eigen::Matrix<double, Rows, Cols> mat;
                for (int r = 0; r < Rows; ++r) {
                    for (int c = 0; c < Cols; ++c) {
                        mat(r, c) = val[r * Cols + c];
                    }
                }
                return mat;
            }
        }
    }
    return default_val;
}

template <int Dim>
inline Eigen::Vector<double, Dim> load_vector(ryml::NodeRef node, const char* name, const Eigen::Vector<double, Dim>& default_val) {
    if (node.has_child(c4::to_csubstr(name))) {
        auto child = node[name];
        if (child.is_seq() && child.num_children() >= Dim) {
            std::vector<double> val(Dim);
            int idx = 0;
            for (auto sub_node : child.children()) {
                if (idx < Dim) {
                    sub_node >> val[idx++];
                }
            }
            if (idx == Dim) {
                Eigen::Vector<double, Dim> vec;
                for (int i = 0; i < Dim; ++i) {
                    vec(i) = val[i];
                }
                return vec;
            }
        }
    }
    return default_val;
}

template <int N, int M, int L = 0>
class KalmanFilter : public core::Component {
public:
    // Static Eigen type definitions
    using StateVector = Eigen::Vector<double, N>;
    using StateMatrix = Eigen::Matrix<double, N, N>;
    using MeasVector  = Eigen::Vector<double, M>;
    using MeasMatrix  = Eigen::Matrix<double, M, N>;
    using MeasNoiseMatrix = Eigen::Matrix<double, M, M>;
    using CtrlVector  = Eigen::Vector<double, L>;
    using CtrlMatrix  = Eigen::Matrix<double, N, L>;

    // Manual/direct constructor for backward compatibility
    KalmanFilter(double dt, const StateMatrix& A, const StateMatrix& Q,
                 const MeasNoiseMatrix& R, const MeasMatrix& H, const CtrlMatrix& B,
                 const CtrlVector& u)
        : dt_(dt), A_(A), Q_(Q), R_(R), H_(H), B_(B), u_(u) {
        x_ = StateVector::Zero();
        P_ = StateMatrix::Identity();
    }

    // Manual constructor without control input (default L = 0)
    KalmanFilter(double dt, const StateMatrix& A, const StateMatrix& Q,
                 const MeasNoiseMatrix& R, const MeasMatrix& H)
        requires(L == 0)
        : dt_(dt), A_(A), Q_(Q), R_(R), H_(H) {
        x_ = StateVector::Zero();
        P_ = StateMatrix::Identity();
        if constexpr (L > 0) {
            B_ = CtrlMatrix::Zero();
            u_ = CtrlVector::Zero();
        }
    }

    // Component constructor
    explicit KalmanFilter(ryml::NodeRef config) {
        auto c = core::Config{config};

        dt_ = c["dt"].get(0.01);
        A_  = load_matrix<N, N>(config, "A", StateMatrix::Identity());
        Q_  = load_matrix<N, N>(config, "Q", StateMatrix::Identity());
        R_  = load_matrix<M, M>(config, "R", MeasNoiseMatrix::Identity());
        H_  = load_matrix<M, N>(config, "H", MeasMatrix::Identity());

        if constexpr (L > 0) {
            B_ = load_matrix<N, L>(config, "B", CtrlMatrix::Zero());
            u_ = load_vector<L>(config, "u", CtrlVector::Zero());
        }

        x_  = load_vector<N>(config, "x0", StateVector::Zero());
        P_  = load_matrix<N, N>(config, "P0", StateMatrix::Identity());

        // Register Inputs
        register_input(c["measurement"].str(), z_in_);
        
        if constexpr (L > 0) {
            register_input(c["control"].str(), u_in_, false);
        }

        // Register Outputs
        register_output(c["state"].str(), state_out_, x_);
        register_output(c["covariance"].str(), covariance_out_, P_);
    }

    void update() override {
        if constexpr (L > 0) {
            if (u_in_.ready()) {
                u_ = *u_in_;
            }
        }

        predict();

        if (z_in_.ready()) {
            update(*z_in_);
        }

        *state_out_      = x_;
        *covariance_out_ = P_;
    }

    void predict() {
        if constexpr (L > 0) {
            x_ = A_ * x_ + B_ * u_;
        } else {
            x_ = A_ * x_;
        }
        P_ = A_ * P_ * A_.transpose() + Q_;
    }

    void update(const MeasVector& z) {
        Eigen::Matrix<double, M, M> S = H_ * P_ * H_.transpose() + R_;
        Eigen::Matrix<double, N, M> K = S.ldlt().solve(P_ * H_.transpose());
        x_ += K * (z - H_ * x_);
        P_ = (StateMatrix::Identity() - K * H_) * P_;
    }

    const StateVector& state() const { return x_; }
    const StateMatrix& covariance() const { return P_; }

    double dt() const { return dt_; }
    void set_dt(double dt) { dt_ = dt; }

    const StateMatrix& A() const { return A_; }
    void set_A(const StateMatrix& A) { A_ = A; }

    const StateMatrix& Q() const { return Q_; }
    void set_Q(const StateMatrix& Q) { Q_ = Q; }

    const MeasNoiseMatrix& R() const { return R_; }
    void set_R(const MeasNoiseMatrix& R) { R_ = R; }

    const MeasMatrix& H() const { return H_; }
    void set_H(const MeasMatrix& H) { H_ = H; }

    const CtrlMatrix& B() const { return B_; }
    void set_B(const CtrlMatrix& B) { B_ = B; }

    const CtrlVector& u() const { return u_; }
    void set_u(const CtrlVector& u) { u_ = u; }

    void set_state(const StateVector& x) { x_ = x; }
    void set_covariance(const StateMatrix& P) { P_ = P; }

private:
    double dt_;
    StateMatrix A_, Q_;
    MeasNoiseMatrix R_;
    MeasMatrix H_;
    
    CtrlMatrix B_;
    CtrlVector u_;
    
    StateVector x_;
    StateMatrix P_;

    InputInterface<MeasVector> z_in_;
    InputInterface<CtrlVector> u_in_;

    OutputInterface<StateVector> state_out_;
    OutputInterface<StateMatrix> covariance_out_;
};

using KalmanFilter2D = KalmanFilter<2, 2, 0>;

} // namespace nuedcs::controller::kalman

REGISTER_COMPONENT(nuedcs::controller::kalman, KalmanFilter2D)

namespace Kalman {
    using KalmanFilter = ::nuedcs::controller::kalman::KalmanFilter<2, 2, 0>;
}