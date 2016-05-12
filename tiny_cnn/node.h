/*
    Copyright (c) 2016, Taiga Nomi
    All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.
    * Neither the name of the <organization> nor the
    names of its contributors may be used to endorse or promote products
    derived from this software without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
    EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
    WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
    DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
    DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
    (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
    LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
    ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
    SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#pragma once
#include <sstream>
#include <iomanip>
#include <memory>
#include <numeric>
#include <vector>
#include <set>

#include "tiny_cnn/util/util.h"
#include "tiny_cnn/util/product.h"
#include "tiny_cnn/util/image.h"
#include "tiny_cnn/util/weight_init.h"
#include "tiny_cnn/optimizers/optimizer.h"

#include "tiny_cnn/activations/activation_function.h"

namespace tiny_cnn {

class layer_base;
class edge;

/**
 * base class of all kind of tinny-cnn data
 **/
class node : public std::enable_shared_from_this<node> {
 public:
    node(cnn_size_t in_size, cnn_size_t out_size)
        : prev_(in_size), next_(out_size) {}
    virtual ~node() {}

    const std::vector<std::shared_ptr<edge>>& prev() const { return prev_; }
    const std::vector<std::shared_ptr<edge>>& next() const { return next_; }

    std::vector<node*> prev_nodes() const; // @todo refactor and remove this method
    std::vector<node*> next_nodes() const; // @todo refactor and remove this method
 protected:
    node() = delete;

    friend void connect_node(std::shared_ptr<node> head,
                             std::shared_ptr<node> tail,
                             cnn_size_t head_index, cnn_size_t tail_index);
    friend void connect(std::shared_ptr<layer_base> head,
                        std::shared_ptr<layer_base> tail,
                        cnn_size_t head_index, cnn_size_t tail_index);

    mutable std::vector<std::shared_ptr<edge>> prev_;
    mutable std::vector<std::shared_ptr<edge>> next_;
};

/**
 * class containing input/output data
 **/
class edge {
 public:
    edge(node* prev, const shape3d& shape, vector_type vtype)
        : worker_specific_data_(!is_trainable_weight(vtype)),
          shape_(shape),
          vtype_(vtype),
          data_(1, vec_t(shape.size())),
          prev_(prev) {
      grad_.resize(1, vec_t(shape.size()));
    }

    void merge_grads(cnn_size_t worker_size, vec_t *dst) {
        *dst = grad_[0];

        for (cnn_size_t i = 1; i < worker_size; i++) {
            vectorize::reduce<float_t>(&grad_[i][0], dst->size(), &(*dst)[0]);
        }
    }

    void clear_grads(cnn_size_t worker_size) {
        for (cnn_size_t i = 0; i < worker_size; i++)
            std::fill(grad_[i].begin(), grad_[i].end(), (float_t)0);
    }

    void set_worker_size(cnn_size_t size) {
        if (worker_specific_data_) data_.resize(size, data_[0]);
        grad_.resize(size, grad_[0]);
    }

    vec_t* get_data(cnn_size_t worker_index = 0) {
        return worker_specific_data_ ? &data_[worker_index] : &data_[0];
    }

    const vec_t* get_data(cnn_size_t worker_index = 0) const {
        return worker_specific_data_ ? &data_[worker_index] : &data_[0];
    }

    vec_t* get_gradient(cnn_size_t worker_index = 0) {
        return &grad_[worker_index];
    }

    const vec_t* get_gradient(cnn_size_t worker_index = 0) const {
        return &grad_[worker_index];
    }

    const std::vector<node*>& next() const { return next_; }
    node* prev() { return prev_; }

    const shape3d& shape() const { return shape_; }
    vector_type vtype() const { return vtype_; }
    void add_next_node(node* next) { next_.push_back(next); }

 private:
    bool worker_specific_data_;
    shape3d shape_;
    vector_type vtype_;
    std::vector<vec_t> data_;
    std::vector<vec_t> grad_;
    node* prev_; // previous node, "producer" of this tensor
    std::vector<node*> next_; // next nodes, "consumers" of this tensor
};

std::vector<node*> node::prev_nodes() const {
    std::set<node*> sets;
    for (auto& e : prev_) {
        if (e && e->prev()) sets.insert(e->prev());
    }
    return std::vector<node*>(sets.begin(), sets.end());
}

std::vector<node*> node::next_nodes() const {
    std::set<node*> sets;
    for (auto& e : next_) {
        if (e) {
            auto n = e->next();
            sets.insert(n.begin(), n.end());
        }
    }
    return std::vector<node*>(sets.begin(), sets.end());
}

struct node_tuple {
    node_tuple(std::shared_ptr<layer_base> l1, std::shared_ptr<layer_base> l2) {
        nodes_.push_back(l1); nodes_.push_back(l2);
    }
    std::vector<std::shared_ptr<layer_base>> nodes_;
};

node_tuple operator , (std::shared_ptr<layer_base> l1,
                       std::shared_ptr<layer_base> l2) {
    return node_tuple(l1, l2);
}

node_tuple operator , (node_tuple* lhs, std::shared_ptr<layer_base> rhs) {
    lhs->nodes_.push_back(rhs);
    return (*lhs);
}

template <typename T, typename U>
inline std::shared_ptr<U>& operator << (std::shared_ptr<T>& lhs,
                                        std::shared_ptr<U>& rhs) {
    connect(lhs, rhs);
    return rhs;
}

template <typename T>
inline std::shared_ptr<T>& operator << (const node_tuple& lhs,
                                        std::shared_ptr<T>& rhs) {
    for (size_t i = 0; i < lhs.nodes_.size(); i++) {
        connect(lhs.nodes_[i], rhs, 0, i);
    }
    return rhs;
}

template <typename T>
inline node_tuple& operator << (std::shared_ptr<T>& lhs,
                                const node_tuple& rhs) {
    for (size_t i = 0; i < rhs.nodes_.size(); i++) {
        connect(lhs, rhs.nodes_[i], i, 0);
    }
    return rhs;
}


}   // namespace tiny_cnn