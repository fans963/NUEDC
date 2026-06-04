#pragma once

#include <Eigen/Dense>

namespace nuedcs::controller::kalman {

template <int N, int M = N, int L = N>
class KalmanFilter {
public:
    using StateVec     = Eigen::Vector<double, N>;
    using MeasVec      = Eigen::Vector<double, M>;
    using ControlVec   = Eigen::Vector<double, L>;
    using StateMat     = Eigen::Matrix<double, N, N>;
    using MeasMat      = Eigen::Matrix<double, M, M>;
    using MeasStateMat = Eigen::Matrix<double, M, N>;
    using StateMeasMat = Eigen::Matrix<double, N, M>;
    using StateCtrlMat = Eigen::Matrix<double, N, L>;

    KalmanFilter() = default;

    KalmanFilter(const StateMat& A, const StateMat& Q, const MeasMat& R, const MeasStateMat& H,
        const StateCtrlMat& B = StateCtrlMat::Zero())
        : A_(A)
        , Q_(Q)
        , R_(R)
        , H_(H)
        , B_(B) { }

    void predict() {
        x_ = A_ * x_;
        P_ = A_ * P_ * A_.transpose() + Q_;
    }

    void predict(const ControlVec& u) {
        x_ = A_ * x_ + B_ * u;
        P_ = A_ * P_ * A_.transpose() + Q_;
    }

    void update(const MeasVec& z) {
        const MeasMat S      = H_ * P_ * H_.transpose() + R_;
        const StateMeasMat K = S.ldlt().solve(H_ * P_).transpose();
        x_ += K * (z - H_ * x_);
        P_ = (StateMat::Identity() - K * H_) * P_;
    }

    void step(const MeasVec& z) {
        predict();
        update(z);
    }

    void step(const ControlVec& u, const MeasVec& z) {
        predict(u);
        update(z);
    }

    const StateVec& state() const { return x_; }
    void set_state(const StateVec& x) { x_ = x; }

    const StateMat& covariance() const { return P_; }
    void set_covariance(const StateMat& P) { P_ = P; }

    const StateMat& A() const { return A_; }
    const StateMat& Q() const { return Q_; }
    const MeasMat& R() const { return R_; }
    const MeasStateMat& H() const { return H_; }
    const StateCtrlMat& B() const { return B_; }

    void set_A(const StateMat& A) { A_ = A; }
    void set_Q(const StateMat& Q) { Q_ = Q; }
    void set_R(const MeasMat& R) { R_ = R; }
    void set_H(const MeasStateMat& H) { H_ = H; }
    void set_B(const StateCtrlMat& B) { B_ = B; }

private:
    StateMat A_     = StateMat::Identity();
    StateMat Q_     = StateMat::Identity();
    MeasMat R_      = MeasMat::Identity();
    MeasStateMat H_ = MeasStateMat::Identity();
    StateCtrlMat B_ = StateCtrlMat::Zero();

    StateVec x_ = StateVec::Zero();
    StateMat P_ = StateMat::Identity();
};

using KalmanFilter2D = KalmanFilter<2, 2, 0>;

} // namespace nuedcs::controller::kalman
