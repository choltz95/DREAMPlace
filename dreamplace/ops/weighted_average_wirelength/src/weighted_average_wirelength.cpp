/**
 * @file   src/weighted_average_wirelength.cpp
 * @author Yibo Lin
 * @date   Jun 2018
 * @brief  Compute weighted-average wirelength and gradient according to e-place
 */
#include "utility/src/torch.h"
#include "utility/src/Msg.h"

DREAMPLACE_BEGIN_NAMESPACE

template <typename T>
int computeWeightedAverageWirelengthLauncher(
    const T *x, const T *y,
    const int *pin2net_map,
    const int *flat_netpin,
    const int *netpin_start,
    const unsigned char *net_mask,
    int num_nets,
    int num_pins,
    const T *inv_gamma,
    T *exp_xy, T *exp_nxy,
    T *exp_xy_sum, T *exp_nxy_sum,
    T *xyexp_xy_sum, T *xyexp_nxy_sum,
    T *wl,
    const T *grad_tensor,
    int num_threads,
    T *grad_x_tensor, T *grad_y_tensor);

/// @brief add net weights to gradient
template <typename T>
void integrateNetWeightsLauncher(
    const int *flat_netpin,
    const int *netpin_start,
    const unsigned char *net_mask,
    const T *net_weights,
    T *grad_x_tensor, T *grad_y_tensor,
    int num_nets,
    int num_threads);

#define CHECK_FLAT(x) AT_ASSERTM(!x.is_cuda() && x.ndimension() == 1, #x " must be a flat tensor on CPU")
#define CHECK_EVEN(x) AT_ASSERTM((x.numel() & 1) == 0, #x " must have even number of elements")
#define CHECK_CONTIGUOUS(x) AT_ASSERTM(x.is_contiguous(), #x " must be contiguous")

/// @brief Compute weighted-average wirelength according to e-place
///     \sum(x*exp(x/gamma)) / \sum(exp(x/gamma)) - \sum(x*exp(-x/gamma)) / \sum(exp(-x/gamma))
/// @param pos cell locations, array of x locations and then y locations
/// @param flat_netpin similar to the JA array in CSR format, which is flattened from the net2pin map (array of array)
/// @param netpin_start similar to the IA array in CSR format, IA[i+1]-IA[i] is the number of pins in each net, the length of IA is number of nets + 1
/// @param net_weights weight of nets
/// @param net_mask an array to record whether compute the where for a net or not
/// @param inv_gamma a scalar tensor for the parameter in the equation
std::vector<at::Tensor> weighted_average_wirelength_forward(
    at::Tensor pos,
    at::Tensor flat_netpin,
    at::Tensor netpin_start,
    at::Tensor net_weights,
    at::Tensor net_mask,
    at::Tensor inv_gamma,
    int num_threads)
{
    CHECK_FLAT(pos);
    CHECK_EVEN(pos);
    CHECK_CONTIGUOUS(pos);
    CHECK_FLAT(flat_netpin);
    CHECK_CONTIGUOUS(flat_netpin);
    CHECK_FLAT(netpin_start);
    CHECK_CONTIGUOUS(netpin_start);
    CHECK_FLAT(net_weights);
    CHECK_CONTIGUOUS(net_weights);
    CHECK_FLAT(net_mask);
    CHECK_CONTIGUOUS(net_mask);

    int num_nets = netpin_start.numel() - 1;
    int num_pins = pos.numel() / 2;

    at::Tensor exp_xy = at::empty_like(pos);
    at::Tensor exp_nxy = at::empty_like(pos);
    at::Tensor exp_xy_sum = at::zeros({2, num_nets}, pos.options());
    at::Tensor exp_nxy_sum = at::zeros({2, num_nets}, pos.options());
    at::Tensor xyexp_xy_sum = at::zeros({2, num_nets}, pos.options());
    at::Tensor xyexp_nxy_sum = at::zeros({2, num_nets}, pos.options());
    at::Tensor wl = at::zeros({num_nets}, pos.options());

    AT_DISPATCH_FLOATING_TYPES(pos.type(), "computeWeightedAverageWirelengthLauncher", [&] {
        computeWeightedAverageWirelengthLauncher<scalar_t>(
            pos.data<scalar_t>(), pos.data<scalar_t>() + pos.numel() / 2,
            nullptr,
            flat_netpin.data<int>(),
            netpin_start.data<int>(),
            net_mask.data<unsigned char>(),
            num_nets,
            num_pins,
            inv_gamma.data<scalar_t>(),
            exp_xy.data<scalar_t>(), exp_nxy.data<scalar_t>(),
            exp_xy_sum.data<scalar_t>(), exp_nxy_sum.data<scalar_t>(),
            xyexp_xy_sum.data<scalar_t>(), xyexp_nxy_sum.data<scalar_t>(),
            wl.data<scalar_t>(),
            nullptr,
            num_threads,
            nullptr, nullptr);
    });
    
    if (net_weights.numel())
    {
        wl.mul_(net_weights);
    }

    return {wl.sum(), exp_xy, exp_nxy, exp_xy_sum, exp_nxy_sum, xyexp_xy_sum, xyexp_nxy_sum};
}

/// @brief Compute gradient
/// @param grad_pos input gradient from backward propagation
/// @param pos locations of pins
/// @param flat_netpin similar to the JA array in CSR format, which is flattened from the net2pin map (array of array)
/// @param netpin_start similar to the IA array in CSR format, IA[i+1]-IA[i] is the number of pins in each net, the length of IA is number of nets + 1
/// @param net_weights weight of nets
/// @param net_mask an array to record whether compute the where for a net or not
/// @param inv_gamma a scalar tensor for the parameter in the equation
at::Tensor weighted_average_wirelength_backward(
    at::Tensor grad_pos,
    at::Tensor pos,
    at::Tensor exp_xy, at::Tensor exp_nxy,
    at::Tensor exp_xy_sum, at::Tensor exp_nxy_sum,
    at::Tensor xyexp_xy_sum, at::Tensor xyexp_nxy_sum,
    at::Tensor flat_netpin,
    at::Tensor netpin_start,
    at::Tensor pin2net_map,
    at::Tensor net_weights,
    at::Tensor net_mask,
    at::Tensor inv_gamma,
    int num_threads)
{
    CHECK_FLAT(pos);
    CHECK_EVEN(pos);
    CHECK_CONTIGUOUS(pos);
    CHECK_FLAT(flat_netpin);
    CHECK_CONTIGUOUS(flat_netpin);
    CHECK_FLAT(netpin_start);
    CHECK_CONTIGUOUS(netpin_start);
    CHECK_FLAT(net_weights);
    CHECK_CONTIGUOUS(net_weights);
    CHECK_FLAT(net_mask);
    CHECK_CONTIGUOUS(net_mask);
    CHECK_FLAT(pin2net_map);
    CHECK_CONTIGUOUS(pin2net_map);

    at::Tensor grad_out = at::zeros_like(pos);

    AT_DISPATCH_FLOATING_TYPES(pos.type(), "computeWeightedAverageWirelengthLauncher", [&] {
        computeWeightedAverageWirelengthLauncher<scalar_t>(
            pos.data<scalar_t>(), pos.data<scalar_t>() + pos.numel() / 2,
            pin2net_map.data<int>(),
            flat_netpin.data<int>(),
            netpin_start.data<int>(),
            net_mask.data<unsigned char>(),
            netpin_start.numel() - 1,
            pos.numel() / 2,
            inv_gamma.data<scalar_t>(),
            exp_xy.data<scalar_t>(), exp_nxy.data<scalar_t>(),
            exp_xy_sum.data<scalar_t>(), exp_nxy_sum.data<scalar_t>(),
            xyexp_xy_sum.data<scalar_t>(), xyexp_nxy_sum.data<scalar_t>(),
            nullptr,
            grad_pos.data<scalar_t>(),
            num_threads,
            grad_out.data<scalar_t>(), grad_out.data<scalar_t>() + pos.numel() / 2);
        if (net_weights.numel())
        {
            integrateNetWeightsLauncher<scalar_t>(
                flat_netpin.data<int>(),
                netpin_start.data<int>(),
                net_mask.data<unsigned char>(),
                net_weights.data<scalar_t>(),
                grad_out.data<scalar_t>(), grad_out.data<scalar_t>() + pos.numel() / 2,
                netpin_start.numel() - 1,
                num_threads);
        }
    });
    return grad_out;
}

template <typename T>
int computeWeightedAverageWirelengthLauncher(
    const T *x, const T *y,
    const int *pin2net_map,
    const int *flat_netpin,
    const int *netpin_start,
    const unsigned char *net_mask,
    int num_nets,
    int num_pins,
    const T *inv_gamma,
    T *exp_xy, T *exp_nxy,
    T *exp_xy_sum, T *exp_nxy_sum,
    T *xyexp_xy_sum, T *xyexp_nxy_sum,
    T *wl,
    const T *grad_tensor,
    int num_threads,
    T *grad_x_tensor, T *grad_y_tensor)
{
    if (grad_tensor)
    {
        int chunk_size = std::max(int(num_pins / num_threads / 16), 1);
#pragma omp parallel for num_threads(num_threads) schedule(dynamic, chunk_size)
        for (int i = 0; i < num_pins; ++i)
        {
            int net_id = pin2net_map[i];
            if (net_mask[net_id])
            {
                grad_x_tensor[i] = (*grad_tensor) * 
                                   (((1 + (*inv_gamma) * x[i]) * exp_xy_sum[net_id] - (*inv_gamma) * xyexp_xy_sum[net_id]) / (exp_xy_sum[net_id] * exp_xy_sum[net_id]) * exp_xy[i] 
                                  - ((1 - (*inv_gamma) * x[i]) * exp_nxy_sum[net_id] + (*inv_gamma) * xyexp_nxy_sum[net_id]) / (exp_nxy_sum[net_id] * exp_nxy_sum[net_id]) * exp_nxy[i]);

                net_id += num_nets;
                int pin_id = i + num_pins;
                grad_y_tensor[i] = (*grad_tensor) * 
                                   (((1 + (*inv_gamma) * y[i]) * exp_xy_sum[net_id] - (*inv_gamma) * xyexp_xy_sum[net_id]) / (exp_xy_sum[net_id] * exp_xy_sum[net_id]) * exp_xy[pin_id] 
                                  - ((1 - (*inv_gamma) * y[i]) * exp_nxy_sum[net_id] + (*inv_gamma) * xyexp_nxy_sum[net_id]) / (exp_nxy_sum[net_id] * exp_nxy_sum[net_id]) * exp_nxy[pin_id]);
            }
        }
    }
    else
    {
        int chunk_size = std::max(int(num_nets / num_threads / 16), 1);
#pragma omp parallel for num_threads(num_threads) schedule(dynamic, chunk_size)
        for (int i = 0; i < num_nets; ++i)
        {
            if (!net_mask[i])
            {
                continue;
            }

            int x_index = i;
            int y_index = i + num_nets;

            //int degree = netpin_start[i+1]-netpin_start[i];
            T x_max = -std::numeric_limits<T>::max();
            T x_min = std::numeric_limits<T>::max();
            T y_max = -std::numeric_limits<T>::max();
            T y_min = std::numeric_limits<T>::max();
            for (int j = netpin_start[i]; j < netpin_start[i + 1]; ++j)
            {
                T xx = x[flat_netpin[j]];
                x_max = std::max(xx, x_max);
                x_min = std::min(xx, x_min);
                T yy = y[flat_netpin[j]];
                y_max = std::max(yy, y_max);
                y_min = std::min(yy, y_min);
            }

            for (int j = netpin_start[i]; j < netpin_start[i + 1]; ++j)
            {
                int pin_id = flat_netpin[j];
                exp_xy[pin_id] = exp((x[pin_id] - x_max) * (*inv_gamma));
                exp_nxy[pin_id] = exp(-(x[pin_id] - x_min) * (*inv_gamma));
                exp_xy_sum[x_index] += exp_xy[pin_id];
                exp_nxy_sum[x_index] += exp_nxy[pin_id];
                xyexp_xy_sum[x_index] += x[pin_id] * exp_xy[pin_id];
                xyexp_nxy_sum[x_index] += x[pin_id] * exp_nxy[pin_id];

                pin_id += num_pins;
                exp_xy[pin_id] = exp((x[pin_id] - y_max) * (*inv_gamma));
                exp_nxy[pin_id] = exp(-(x[pin_id] - y_min) * (*inv_gamma));
                exp_xy_sum[y_index] += exp_xy[pin_id];
                exp_nxy_sum[y_index] += exp_nxy[pin_id];
                xyexp_xy_sum[y_index] += x[pin_id] * exp_xy[pin_id];
                xyexp_nxy_sum[y_index] += x[pin_id] * exp_nxy[pin_id];
            }

            wl[i] = xyexp_xy_sum[x_index] / exp_xy_sum[x_index] - xyexp_nxy_sum[x_index] / exp_nxy_sum[x_index] +
                    xyexp_xy_sum[y_index] / exp_xy_sum[y_index] - xyexp_nxy_sum[y_index] / exp_nxy_sum[y_index];
        }
    }

    return 0;
}

template <typename T>
void integrateNetWeightsLauncher(
    const int *flat_netpin,
    const int *netpin_start,
    const unsigned char *net_mask,
    const T *net_weights,
    T *grad_x_tensor, T *grad_y_tensor,
    int num_nets,
    int num_threads)
{
#pragma omp parallel for num_threads(num_threads)
    for (int net_id = 0; net_id < num_nets; ++net_id)
    {
        if (net_mask[net_id])
        {
            T weight = net_weights[net_id];
            for (int j = netpin_start[net_id]; j < netpin_start[net_id + 1]; ++j)
            {
                int pin_id = flat_netpin[j];
                grad_x_tensor[pin_id] *= weight;
                grad_y_tensor[pin_id] *= weight;
            }
        }
    }
}

DREAMPLACE_END_NAMESPACE

PYBIND11_MODULE(TORCH_EXTENSION_NAME, m)
{
    m.def("forward", &DREAMPLACE_NAMESPACE::weighted_average_wirelength_forward, "WeightedAverageWirelength forward");
    m.def("backward", &DREAMPLACE_NAMESPACE::weighted_average_wirelength_backward, "WeightedAverageWirelength backward");
}
