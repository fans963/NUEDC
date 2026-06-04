#pragma once

#include <Eigen/Dense>
#include <chrono>
#include <optional>

namespace nuedcs::controller::kalman {

template <typename T>
concept MotionModel = requires(const T& m, double dt) {
    m.A(dt);
    m.B(dt);
    m.Q(dt);
};

template <int N, int M, int L, typename Model>
    requires MotionModel<Model>
class KalmanFilter {
public:
    using Clock        = std::chrono::steady_clock;
    using StateVec     = Eigen::Vector<double, N>;
    using MeasVec      = Eigen::Vector<double, M>;
    using ControlVec   = Eigen::Vector<double, L>;
    using StateMat     = Eigen::Matrix<double, N, N>;
    using MeasMat      = Eigen::Matrix<double, M, M>;
    using MeasStateMat = Eigen::Matrix<double, M, N>;
    using StateMeasMat = Eigen::Matrix<double, N, M>;
    using StateCtrlMat = Eigen::Matrix<double, N, L>;

    explicit KalmanFilter(Model model, double dt = 0.01)
        : model_(model)
        , dt_(dt) { }

    void predict() {
        const double d = advance_clock();
        x_             = model_.A(d) * x_;
        P_             = model_.A(d) * P_ * model_.A(d).transpose() + model_.Q(d);
    }

    void predict(const ControlVec& u) {
        const double d = advance_clock();
        x_             = model_.A(d) * x_ + model_.B(d) * u;
        P_             = model_.A(d) * P_ * model_.A(d).transpose() + model_.Q(d);
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

    double dt() const { return dt_; }
    double t() const { return t_; }

    const MeasStateMat& H() const { return H_; }
    const MeasMat& R() const { return R_; }
    void set_H(const MeasStateMat& H) { H_ = H; }
    void set_R(const MeasMat& R) { R_ = R; }

    Model& model() { return model_; }
    const Model& model() const { return model_; }

private:
    double advance_clock() {
        const auto now = Clock::now();
        if (last_time_.has_value()) dt_ = std::chrono::duration<double>(now - *last_time_).count();
        last_time_ = now;
        t_ += dt_;
        return dt_;
    }

    Model model_;
    double dt_ = 0.01;
    double t_  = 0.0;
    std::optional<Clock::time_point> last_time_;

    MeasStateMat H_ = MeasStateMat::Identity();
    MeasMat R_      = MeasMat::Identity();

    StateVec x_ = StateVec::Zero();
    StateMat P_ = StateMat::Identity();
};

template <int N, int M = N, int L = N>
struct ConstModel {
    using StateMat     = Eigen::Matrix<double, N, N>;
    using StateCtrlMat = Eigen::Matrix<double, N, L>;

    StateMat A_     = StateMat::Identity();
    StateCtrlMat B_ = StateCtrlMat::Zero();
    StateMat Q_     = StateMat::Identity();

    [[nodiscard]] const StateMat& A(double) const { return A_; }
    [[nodiscard]] const StateCtrlMat& B(double) const { return B_; }
    [[nodiscard]] const StateMat& Q(double) const { return Q_; }
};

template <int N, int M = N, int L = N>
auto make_kalman_filter(double dt = 0.01) {
    return KalmanFilter<N, M, L, ConstModel<N, M, L>>(ConstModel<N, M, L> { }, dt);
}

using KalmanFilter2D = KalmanFilter<2, 2, 2, ConstModel<2, 2, 2>>;

} // namespace nuedcs::controller::kalman
