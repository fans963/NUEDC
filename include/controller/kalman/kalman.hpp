#pragma once

#include "core/component.hpp"
#include "core/component_registry.hpp"
#include <Eigen/Dense>
#include <ryml/ryml.hpp>

namespace nuedcs::controller::kalman {

inline Eigen::Matrix2d load_matrix2d(ryml::NodeRef node, const char* name, const Eigen::Matrix2d& default_val) {
    if (node.has_child(c4::to_csubstr(name))) {
        auto child = node[name];
        if (child.is_seq() && child.num_children() >= 4) {
            double val[4];
            int idx = 0;
            for (auto sub_node : child.children()) {
                if (idx < 4) {
                    sub_node >> val[idx++];
                }
            }
            if (idx == 4) {
                Eigen::Matrix2d mat;
                mat << val[0], val[1], val[2], val[3];
                return mat;
            }
        }
    }
    return default_val;
}

inline Eigen::Vector2d load_vector2d(ryml::NodeRef node, const char* name, const Eigen::Vector2d& default_val) {
    if (node.has_child(c4::to_csubstr(name))) {
        auto child = node[name];
        if (child.is_seq() && child.num_children() >= 2) {
            double val[2];
            int idx = 0;
            for (auto sub_node : child.children()) {
                if (idx < 2) {
                    sub_node >> val[idx++];
                }
            }
            if (idx == 2) {
                Eigen::Vector2d vec;
                vec << val[0], val[1];
                return vec;
            }
        }
    }
    return default_val;
}

class KalmanFilter : public core::Component {
public:
    // Manual/direct constructor for backward compatibility
    KalmanFilter(double dt, const Eigen::Matrix2d& A, const Eigen::Matrix2d& Q,
                 const Eigen::Matrix2d& R, const Eigen::Matrix2d& H, const Eigen::Matrix2d& B,
                 const Eigen::Vector2d& u)
        : dt_(dt), A_(A), Q_(Q), R_(R), H_(H), B_(B), u_(u) {
        x_ = Eigen::Vector2d::Zero();
        P_ = Eigen::Matrix2d::Identity();
    }

    // Component constructor
    explicit KalmanFilter(ryml::NodeRef config) {
        auto c = core::Config{config};

        dt_ = c["dt"].get(0.01);
        A_  = load_matrix2d(config, "A", Eigen::Matrix2d::Identity());
        Q_  = load_matrix2d(config, "Q", Eigen::Matrix2d::Identity());
        R_  = load_matrix2d(config, "R", Eigen::Matrix2d::Identity());
        H_  = load_matrix2d(config, "H", Eigen::Matrix2d::Identity());
        B_  = load_matrix2d(config, "B", Eigen::Matrix2d::Zero());
        u_  = load_vector2d(config, "u", Eigen::Vector2d::Zero());

        x_  = load_vector2d(config, "x0", Eigen::Vector2d::Zero());
        P_  = load_matrix2d(config, "P0", Eigen::Matrix2d::Identity());

        // Register Inputs
        register_input(c["measurement"].str(), z_in_);
        register_input(c["control"].str(), u_in_, false);

        // Register Outputs
        register_output(c["state"].str(), state_out_, x_);
        register_output(c["covariance"].str(), covariance_out_, P_);
    }

    void update() override {
        if (u_in_.ready()) {
            u_ = *u_in_;
        }

        predict();

        if (z_in_.ready()) {
            update(*z_in_);
        }

        *state_out_      = x_;
        *covariance_out_ = P_;
    }

    void predict() {
        x_ = A_ * x_ + B_ * u_;
        P_ = A_ * P_ * A_.transpose() + Q_;
    }

    void update(const Eigen::Vector2d& z) {
        Eigen::Matrix2d S = H_ * P_ * H_.transpose() + R_;
        Eigen::Matrix2d K = S.ldlt().solve(P_ * H_.transpose());
        x_ += K * (z - H_ * x_);
        P_ = (Eigen::Matrix2d::Identity() - K * H_) * P_;
    }

    const Eigen::Vector2d& state() const { return x_; }
    const Eigen::Matrix2d& covariance() const { return P_; }

    double dt() const { return dt_; }
    void set_dt(double dt) { dt_ = dt; }

    const Eigen::Matrix2d& A() const { return A_; }
    void set_A(const Eigen::Matrix2d& A) { A_ = A; }

    const Eigen::Matrix2d& Q() const { return Q_; }
    void set_Q(const Eigen::Matrix2d& Q) { Q_ = Q; }

    const Eigen::Matrix2d& R() const { return R_; }
    void set_R(const Eigen::Matrix2d& R) { R_ = R; }

    const Eigen::Matrix2d& H() const { return H_; }
    void set_H(const Eigen::Matrix2d& H) { H_ = H; }

    const Eigen::Matrix2d& B() const { return B_; }
    void set_B(const Eigen::Matrix2d& B) { B_ = B; }

    const Eigen::Vector2d& u() const { return u_; }
    void set_u(const Eigen::Vector2d& u) { u_ = u; }

    void set_state(const Eigen::Vector2d& x) { x_ = x; }
    void set_covariance(const Eigen::Matrix2d& P) { P_ = P; }

private:
    double dt_;
    Eigen::Matrix2d A_, Q_, R_, H_, B_;
    Eigen::Vector2d x_, u_;
    Eigen::Matrix2d P_;

    InputInterface<Eigen::Vector2d> z_in_;
    InputInterface<Eigen::Vector2d> u_in_;

    OutputInterface<Eigen::Vector2d> state_out_;
    OutputInterface<Eigen::Matrix2d> covariance_out_;
};

} // namespace nuedcs::controller::kalman

REGISTER_COMPONENT(nuedcs::controller::kalman, KalmanFilter)

namespace Kalman {
    using KalmanFilter = ::nuedcs::controller::kalman::KalmanFilter;
}