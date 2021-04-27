#include <cmath>
#include <iomanip>
#include <sstream>

#include "epke_solver.hpp"
#include "utility/interpolate.hpp"

using namespace epke;

Solver::Solver(const pugi::xml_node& input_node,
	       const pugi::xml_node& output_node)
  : params(input_node),
    precomp(output_node),
    power(params.getNumTimeSteps()),
    rho(params.getNumTimeSteps()),
    omega(params.getNumPrecursors()),
    zeta_hat(params.getNumPrecursors()),
    concentrations(params.getNumPrecursors(),
		   timeBins(params.getNumTimeSteps())) {
  // set the initial concentration histories
  for (int k = 0; k < params.getNumPrecursors(); k++) {
    for (int n = 0; n < precomp.getNumTimeSteps(); n++) {
      concentrations[k][n] = precomp.getConcentration(k,n);
    }
  }

  // set the initial power histories and rho histories
  for (int n = 0; n < precomp.getNumTimeSteps(); n++) {
    power[n] = precomp.getPower(n);
    rho[n] = precomp.getRho(n);
  }
}

Solver::Solver(const EPKEParameters& params, const EPKEOutput& precomp)
  : params(params),
    precomp(precomp),
    power(params.getNumTimeSteps()),
    rho(params.getNumTimeSteps()),
    omega(params.getNumPrecursors()),
    zeta_hat(params.getNumPrecursors()),
    concentrations(params.getNumPrecursors(),
		   timeBins(params.getNumTimeSteps())) {
  // set the initial concentration histories
  for (int k = 0; k < params.getNumPrecursors(); k++) {
    for (int n = 0; n < precomp.getNumTimeSteps(); n++) {
      concentrations[k][n] = precomp.getConcentration(k,n);
    }
  }

  // set the initial power histories and rho histories
  for (int n = 0; n < precomp.getNumTimeSteps(); n++) {
    power[n] = precomp.getPower(n);
    rho[n] = precomp.getRho(n);
  }
}

Solver::ptr Solver::createFineSolver(const timeBins& fine_time,
				     const timeIndex coarse_index) {
  auto fine_solver =
    std::make_shared<Solver>(params.interpolate(fine_time),
			     precomp.createPrecomputed(coarse_index));
  // Push back the coarse solver's vector of fine solvers
  _fine_solvers.push_back(fine_solver);

  return fine_solver;
}

const para::SolverOutput::ptr Solver::assembleGlobalOutput() const {
  // TEMP
  return std::make_shared<para::SolverOutput>(precomp);
}

const double Solver::computeDT(const timeIndex n) const {
  return params.getTime(n) - params.getTime(n-1);
}

const double Solver::computeGamma(const timeIndex n) const {
  return n < 2 ? 1.0 : computeDT(n-1) / computeDT(n);
}

const double Solver::computeOmega(const precIndex k,
				  const timeIndex n,
				  const double    w,
				  const double    gamma) const {
  const auto lambda_k = params.getDecayConstant(k,n);
  const auto dt       = computeDT(n);
  return params.getGenTime(0) / params.getGenTime(n) *
    params.getDelayedFraction(k,n) * w *
    (util::k2(lambda_k, dt) +
     gamma * dt * util::k1(lambda_k, dt)) / ((1 + gamma) * dt * dt);
}

const double Solver::computeZetaHat(const precIndex k,
				    const timeIndex n,
				    const double    w,
				    const double    gamma) const {
  double beta_prev_prev, power_prev_prev, gen_time_prev_prev;

  const auto lambda_k = params.getDecayConstant(k,n);
  const auto dt       = computeDT(n);

  if (n < 2) {
    beta_prev_prev = params.getDelayedFraction(k,n-1);
    power_prev_prev = power.at(n-1);
    gen_time_prev_prev = params.getGenTime(n-1);
  } else {
    beta_prev_prev = params.getDelayedFraction(k,n-2);
    power_prev_prev = power.at(n-2);
    gen_time_prev_prev = params.getGenTime(n-2);
  }

  return w * concentrations.at(k).at(n - 1) +
    w * params.getGenTime(0) * power.at(n - 1) *
    params.getDelayedFraction(k,n-1) / params.getGenTime(n - 1) *
    (util::k0(lambda_k, dt) - (util::k2(lambda_k, dt) - dt * (gamma - 1) *
			       util::k1(lambda_k, dt)) /
     (gamma * dt * dt)) +
    w * params.getGenTime(0) * power_prev_prev * beta_prev_prev /
    gen_time_prev_prev *
    (util::k2(lambda_k, dt) - dt * util::k1(lambda_k, dt)) /
    ((1 + gamma) * gamma * dt * dt);
}

std::pair<double, double> Solver::computeA1B1(const timeIndex n,
					      const double    gamma) {
  using util::k0, util::k1, util::k2, util::E;

  double H_prev_prev;
  // define local variables to avoid numerous function calls
  const double lh = params.getLambdaH(n);
  const double dt = computeDT(n);

  // we have to set these values so we don't get an out of range vector
  if (n < 2) {
    H_prev_prev = params.getPowNorm(n-1) * power.at(n - 1);
  } else {
    H_prev_prev = params.getPowNorm(n-2) * power.at(n - 2);
  }

  double a1 = params.getGammaD() * params.getPowNorm(n) /
    E(lh, dt) * (k2(lh, dt) + k1(lh, dt) * gamma * dt) / ((1 + gamma) * dt * dt);
  double b1 = params.getRhoImp(n) + 1 / E(lh, dt) *
    ((rho.at(n-1) - params.getRhoImp(n-1)) - power.at(0) * params.getGammaD() *
     params.getEta() * k0(lh, dt)) + params.getGammaD() / E(lh, dt) *
    (params.getPowNorm(n-1) * power.at(n - 1) *
     (k0(lh, dt) - (k2(lh, dt) + (gamma - 1) * dt * k1(lh, dt)) /
      (gamma * dt * dt)) + H_prev_prev * (k2(lh, dt) - k1(lh, dt) * dt) /
     ((1 + gamma) * gamma * dt * dt));;

  return std::make_pair(a1, b1);
}

const double Solver::computePower(const timeIndex n,
				  const double alpha,
				  const double gamma) {
  double tau = 0.0, s_hat_d = 0.0, s_d_prev = 0.0;
  for (int k = 0; k < params.getNumPrecursors(); k++) {
    double w = 1 / util::E(params.getDecayConstant(k,n), computeDT(n));

    omega[k] = computeOmega(k, n, w, gamma);
    zeta_hat[k] = computeZetaHat(k, n, w, gamma);

    // accumulate the weighted sum
    tau += params.getDecayConstant(k,n) * omega.at(k);
    s_hat_d += params.getDecayConstant(k,n) * zeta_hat.at(k);
    s_d_prev += params.getDecayConstant(k,n-1) * concentrations.at(k).at(n-1);
  }

  std::pair<double, double> a1b1 = computeA1B1(n, gamma);

  return computeABC(n, alpha, a1b1, tau, s_hat_d, s_d_prev);
}

const double Solver::computeABC(const timeIndex n,
				const double alpha,
				const std::pair<double, double>& a1b1,
				const double tau, const double s_hat_d,
				const double s_d_prev) const {
  const auto dt = computeDT(n);
  double a = params.getTheta() * dt * a1b1.first / params.getGenTime(n);
  double b = params.getTheta() * dt * (((a1b1.second - params.getBetaEff(n))
					/ params.getGenTime(n) - alpha) +
				       tau / params.getGenTime(0)) - 1;
  double c = params.getTheta() * dt / params.getGenTime(0) * s_hat_d +
    exp(alpha * dt) * ((1 - params.getTheta()) * dt *
		       (((rho.at(n - 1) - params.getBetaEff(n - 1)) /
			 params.getGenTime(n - 1) -
			 alpha) * power.at(n - 1) + s_d_prev /
			params.getGenTime(0)) +
		       power.at(n - 1));

  if (a < 0) {
    return (-b - sqrt(b * b - 4 * a * c)) / (2 * a);
  } else if (a == 0) {
    return -c / b;
  } else {
    throw;
  }
}

// returns true if the transformation criterion is met
const bool Solver::acceptTransformation(const timeIndex n,
					const double alpha,
					const double gamma) const {

  const double power_prev_prev = n < 2 ? power.at(n - 1) : power.at(n - 2);

  double lhs = fabs(power.at(n) - exp(alpha * computeDT(n)) * power.at(n - 1));
  double rhs = fabs(
      power.at(n) - power.at(n - 1) -
      (power.at(n - 1) - power_prev_prev) / gamma);
  return (lhs <= rhs);
}

para::SolverOutput::ptr Solver::solve() {
  // set initial conditions for the power and reactivity vectors
  double alpha = 0.0;

  for (int n = precomp.getNumTimeSteps(); n < params.getNumTimeSteps(); n++) {
    double gamma = computeGamma(n);

    // compute the transformation parameter
    if (n > 1) {
      alpha = 1 / computeDT(n - 1) * log(power.at(n - 1) / power.at(n - 2));
    }

    // evaluate the power at this time step
    power[n] = computePower(n, alpha, gamma);

    // test whether we accept or reject the transformation parameter
    // if (!acceptTransformation(n, alpha, gamma)) {
    // alpha = 0.0;
    // power[n] = computeABC(n, alpha);
    //}

    // update the precursor concentrations
    for (int k = 0; k < params.getNumPrecursors(); k++) {
      concentrations[k][n] = power.at(n) * omega.at(k) + zeta_hat.at(k);
    }

    std::pair<double, double> a1b1 = computeA1B1(n, gamma);

    // update the full power and reactivity vectors
    rho[n] = a1b1.first * power.at(n) + a1b1.second;
  }

  return std::make_shared<EPKEOutput>(power, rho, concentrations);
}

void Solver::buildXMLDoc(pugi::xml_document& doc) const {
  pugi::xml_node output_node = doc.append_child("epke_output");
  pugi::xml_node time_node = output_node.append_child("time");
  pugi::xml_node power_node = output_node.append_child("power");
  pugi::xml_node rho_node = output_node.append_child("rho");
  pugi::xml_node concs_node = output_node.append_child("concentrations");
  std::ostringstream time_str, power_str, rho_str, conc_str;

  for (int n = 0; n < params.getNumTimeSteps(); n++) {
    time_str << std::setprecision(6) << params.getTime(n);
    power_str << std::setprecision(12) << params.getPowNorm(n) * power.at(n);
    rho_str << std::setprecision(12) << rho.at(n);
    if (n != params.getNumTimeSteps() - 1) {
      time_str << " ";
      power_str << " ";
      rho_str << " ";
    }
  }

  time_node.text() = time_str.str().c_str();
  power_node.text() = power_str.str().c_str();
  rho_node.text() = rho_str.str().c_str();

  for (int k = 0; k < params.getNumPrecursors(); k++) {
    conc_str.str("");
    conc_str.clear();
    pugi::xml_node conc_node = concs_node.append_child("concentration");
    conc_node.append_attribute("k") = k;
    for (int n = 0; n < params.getNumTimeSteps(); n++) {
      conc_str << std::setprecision(12) << concentrations.at(k).at(n);

      if (n != params.getNumTimeSteps() - 1) {
        conc_str << " ";
      }
    }
    conc_node.text() = conc_str.str().c_str();
  }
}
