/**
 * @file   pin_pos.cpp
 * @author Yibo Lin
 * @date   Aug 2019
 * @brief  Given cell locations, compute pin locations on CPU 
 */
#include "utility/src/torch.h"
#include "utility/src/Msg.h"

DREAMPLACE_BEGIN_NAMESPACE

/// @brief Given cell locations, compute pin locations
/// @param x cell locations in x direction
/// @param y cell locations in y direction
/// @param pin_offset_x pin offset in x direction 
/// @param pin_offset_y pin offset in y direction
/// @param pin2node_map map pin index to node index 
/// @param flat_node2pin_map map node index to pins 
/// @param flat_node2pin_start_map start index of flat_node2pin_map for each node
/// @param num_pins number of pins 
/// @param num_threads number of threads 
/// @param pin_x pin positions in x direction 
/// @param pin_y pin positions in y direction 
template <typename T>
int computePinPosLauncher(
        const T* x, const T* y, 
        const T* pin_offset_x, 
        const T* pin_offset_y, 
        const long* pin2node_map, 
        const int* flat_node2pin_map, 
        const int* flat_node2pin_start_map, 
        int num_pins, 
        const int num_threads, 
        T* pin_x, T* pin_y
        );

template <typename T>
int computePinPosGradLauncher(
        const T* grad_out_x, const T* grad_out_y, 
        const T* x, const T* y, 
        const T* pin_offset_x, 
        const T* pin_offset_y, 
        const long* pin2node_map, 
        const int* flat_node2pin_map, 
        const int* flat_node2pin_start_map, 
        int num_nodes, 
        int num_pins, 
        const int num_threads, 
        T* grad, T* grad_y
        );

#define CHECK_FLAT(x) AT_ASSERTM(!x.is_cuda() && x.ndimension() == 1, #x "must be a flat tensor on CPU")
#define CHECK_EVEN(x) AT_ASSERTM((x.numel()&1) == 0, #x "must have even number of elements")
#define CHECK_CONTIGUOUS(x) AT_ASSERTM(x.is_contiguous(), #x "must be contiguous")

/// @brief Given cell locations, compute pin locations
/// @param pos cell locations in x and then y direction
/// @param pin_offset_x pin offset in x direction 
/// @param pin_offset_y pin offset in y direction
/// @param pin2node_map map pin index to node index 
/// @param flat_node2pin_map map node index to pins 
/// @param flat_node2pin_start_map start index of flat_node2pin_map for each node
/// @param num_nodes number of nodes 
/// @param num_pins number of pins 
/// @param num_threads number of threads 
/// @return pin positions in x and then y direction 
at::Tensor pin_pos_forward(
        at::Tensor pos,
        at::Tensor pin_offset_x, 
        at::Tensor pin_offset_y, 
        at::Tensor pin2node_map, 
        at::Tensor flat_node2pin_map, 
        at::Tensor flat_node2pin_start_map, 
        int num_threads
        ) 
{
    CHECK_FLAT(pos); 
    CHECK_EVEN(pos);
    CHECK_CONTIGUOUS(pos);

    auto out = at::zeros(pin_offset_x.numel()*2, pos.options());
    int num_nodes = pos.numel()/2;
    int num_pins = pin_offset_x.numel();

    AT_DISPATCH_FLOATING_TYPES(pos.type(), "computePinPosLauncher", [&] {
            computePinPosLauncher<scalar_t>(
                    pos.data<scalar_t>(), pos.data<scalar_t>()+num_nodes, 
                    pin_offset_x.data<scalar_t>(), 
                    pin_offset_y.data<scalar_t>(), 
                    pin2node_map.data<long>(), 
                    flat_node2pin_map.data<int>(), 
                    flat_node2pin_start_map.data<int>(),  
                    num_pins, 
                    num_threads, 
                    out.data<scalar_t>(), out.data<scalar_t>()+num_pins
                    );
            });

    return out;
}

at::Tensor pin_pos_backward(
        at::Tensor grad_out, 
        at::Tensor pos,
        at::Tensor pin_offset_x, 
        at::Tensor pin_offset_y, 
        at::Tensor pin2node_map, 
        at::Tensor flat_node2pin_map, 
        at::Tensor flat_node2pin_start_map, 
        int num_physical_nodes, 
        int num_threads
        ) 
{
    CHECK_FLAT(pos); 
    CHECK_EVEN(pos);
    CHECK_CONTIGUOUS(pos);
    CHECK_FLAT(grad_out);
    CHECK_EVEN(grad_out);
    CHECK_CONTIGUOUS(grad_out);

    auto out = at::zeros_like(pos);
    int num_nodes = pos.numel()/2;
    int num_pins = pin_offset_x.numel();

    AT_DISPATCH_FLOATING_TYPES(pos.type(), "computePinPosGradLauncher", [&] {
            computePinPosGradLauncher<scalar_t>(
                    grad_out.data<scalar_t>(), grad_out.data<scalar_t>()+num_pins, 
                    pos.data<scalar_t>(), pos.data<scalar_t>()+num_nodes, 
                    pin_offset_x.data<scalar_t>(), 
                    pin_offset_y.data<scalar_t>(), 
                    pin2node_map.data<long>(), 
                    flat_node2pin_map.data<int>(), 
                    flat_node2pin_start_map.data<int>(),  
                    num_physical_nodes, 
                    num_pins, 
                    num_threads, 
                    out.data<scalar_t>(), out.data<scalar_t>()+num_nodes
                    );
            });

    return out;
}

/// @brief Given cell locations, compute pin locations
/// @param x cell locations in x direction
/// @param y cell locations in y direction
/// @param pin_offset_x pin offset in x direction 
/// @param pin_offset_y pin offset in y direction
/// @param pin2node_map map pin index to node index 
/// @param flat_node2pin_map map node index to pins 
/// @param flat_node2pin_start_map start index of flat_node2pin_map for each node
/// @param num_nodes number of nodes 
/// @param num_pins number of pins 
/// @param num_threads number of threads 
/// @param pin_x pin positions in x direction 
/// @param pin_y pin positions in y direction 
template <typename T>
int computePinPosLauncher(
        const T* x, const T* y, 
        const T* pin_offset_x, 
        const T* pin_offset_y, 
        const long* pin2node_map, 
        const int* flat_node2pin_map, 
        const int* flat_node2pin_start_map, 
        int num_pins, 
        const int num_threads, 
        T* pin_x, T* pin_y
        )
{
    // density_map_tensor should be initialized outside 
    
#pragma omp parallel for num_threads(num_threads)
    for (int i = 0; i < num_pins; ++i)
    {
        int node_id = pin2node_map[i]; 
        pin_x[i] = pin_offset_x[i] + x[node_id]; 
        pin_y[i] = pin_offset_y[i] + y[node_id];
    }

    return 0; 
}

template <typename T>
int computePinPosGradLauncher(
        const T* grad_out_x, const T* grad_out_y, 
        const T* x, const T* y, 
        const T* pin_offset_x, 
        const T* pin_offset_y, 
        const long* pin2node_map, 
        const int* flat_node2pin_map, 
        const int* flat_node2pin_start_map, 
        int num_nodes, 
        int num_pins, 
        const int num_threads, 
        T* grad_x, T* grad_y
        )
{
#pragma omp parallel for num_threads(num_threads)
    for (int i = 0; i < num_nodes; ++i)
    {
        int bgn = flat_node2pin_start_map[i]; 
        int end = flat_node2pin_start_map[i+1];
        T& gx = grad_x[i]; 
        T& gy = grad_y[i];
        for (int j = bgn; j < end; ++j)
        {
            int pin_id = flat_node2pin_map[j]; 
            gx += grad_out_x[pin_id];
            gy += grad_out_y[pin_id];
        }
    }

    return 0; 
}

DREAMPLACE_END_NAMESPACE

PYBIND11_MODULE(TORCH_EXTENSION_NAME, m) {
  m.def("forward", &DREAMPLACE_NAMESPACE::pin_pos_forward, "PinPos forward");
  m.def("backward", &DREAMPLACE_NAMESPACE::pin_pos_backward, "PinPos backward");
}
