#include <gtest/gtest.h>

#include "simulate_vector.h"

#include "build_snn_graph.hpp"

#include <random>
#include <cmath>
#include <map>
#include <deque>

class BuildSnnGraphTestCore {
protected:
    inline static int ndim, nobs;
    inline static std::vector<double> data;

    static void assemble(int nd, int no) {
        ndim = nd;
        nobs = no;
        data = simulate_vector(ndim * nobs, /* lower = */ 0.0, /* upper = */ 1.0, /* seed = */ ndim + nobs * 20);
    }

    typedef std::tuple<int, int, double> WeightedEdge;

    static std::vector<WeightedEdge> convert_to_vector(const scran::build_snn_graph::Results<int, double>& res) {
        std::vector<WeightedEdge> expected;
        expected.reserve(res.weights.size());
        for (size_t e = 0; e < res.weights.size(); ++e) {
            expected.emplace_back(res.edges[e*2], res.edges[e*2+1], res.weights[e]);
        }
        return expected;
    }
};

/*****************************************
 *****************************************/

class BuildSnnGraphRefTest : 
    public ::testing::TestWithParam<std::tuple<int, scran::build_snn_graph::Scheme, int> >, 
    public BuildSnnGraphTestCore 
{
protected:
    static void SetUpTestSuite() {
        assemble(10, 200); // dimensions, observations.
    }

    static std::vector<WeightedEdge> reference(int k, scran::build_snn_graph::Scheme scheme) {
        std::vector<WeightedEdge> output;
        knncolle::VptreePrebuilt<knncolle::EuclideanDistance, int, int, double, double> prebuilt(ndim, nobs, data);
        auto nnidx = prebuilt.initialize();
        auto ncells = prebuilt.num_observations();

        std::vector<int> neighbors;
        std::vector<int> neighbors_of_neighbors;

        for (int i = 0; i < ncells; ++i) {
            nnidx->search(i, k, &neighbors, NULL);
            std::map<int, double> rankings;
            rankings[i] = 0;
            for (size_t r = 1; r <= neighbors.size(); ++r) {
                rankings[neighbors[r-1]] = r;
            }

            for (int j = 0; j < i; ++j) {
                nnidx->search(j, k, &neighbors_of_neighbors, NULL);
                double weight = 0;
                bool found = false;

                for (size_t r = 0; r <= neighbors_of_neighbors.size(); ++r) {
                    size_t candidate = (r == 0 ? j : static_cast<size_t>(neighbors_of_neighbors[r-1]));
                    auto rIt = rankings.find(candidate);
                    if (rIt != rankings.end()) {
                        if (scheme == scran::build_snn_graph::Scheme::RANKED) {
                            double otherrank = rIt->second + r;
                            if (!found || otherrank < weight) {
                                weight = otherrank;
                            }
                        } else {
                            ++weight;
                            found = true;
                        }
                        found = true;
                    }
                }

                if (found) {
                    if (scheme == scran::build_snn_graph::Scheme::RANKED) {
                        weight = static_cast<double>(neighbors.size()) - 0.5 * weight;
                    } else if (scheme == scran::build_snn_graph::Scheme::JACCARD) {
                        weight /= (2 * (neighbors.size() + 1) - weight);
                    }
                    output.emplace_back(i, j, std::max(weight, 1e-6));
                }
            }
        }

        return output;
    }
};

TEST_P(BuildSnnGraphRefTest, Reference) {
    auto param = GetParam();
    int k = std::get<0>(param);
    auto scheme = std::get<1>(param);
    int nthreads = std::get<2>(param);

    scran::build_snn_graph::Options opts;
    opts.num_neighbors = k;
    opts.weighting_scheme = scheme;
    opts.num_threads = nthreads;
    knncolle::VptreeBuilder<> vpbuilder;
    auto res = scran::build_snn_graph::compute(ndim, nobs, data.data(), vpbuilder, opts);

    auto ref = reference(k, scheme);
    EXPECT_EQ(res.num_cells, nobs);
    EXPECT_EQ(res.edges.size(), 2 * ref.size());
    EXPECT_EQ(res.weights.size(), ref.size());

    auto expected = convert_to_vector(res);
    std::sort(expected.begin(), expected.end());
    std::sort(ref.begin(), ref.end());
    EXPECT_EQ(expected, ref);
}

INSTANTIATE_TEST_SUITE_P(
    BuildSnnGraph,
    BuildSnnGraphRefTest,
    ::testing::Combine(
        ::testing::Values(2, 5, 10), // number of neighbors
        ::testing::Values(scran::build_snn_graph::Scheme::RANKED, scran::build_snn_graph::Scheme::NUMBER, scran::build_snn_graph::Scheme::JACCARD), // weighting scheme
        ::testing::Values(1, 3) // number of threads
    )
);

/*****************************************
 *****************************************/

class BuildSnnGraphSymmetryTest : public ::testing::TestWithParam<std::tuple<int, scran::build_snn_graph::Scheme> >, public BuildSnnGraphTestCore {
protected:
    static void SetUpTestSuite() {
        assemble(5, 1000); // dimensions, observations.
    }
};

TEST_P(BuildSnnGraphSymmetryTest, Symmetry) {
    auto param = GetParam();
    int k = std::get<0>(param);
    auto scheme = std::get<1>(param);

    scran::build_snn_graph::Options opts;
    opts.num_neighbors = k;
    opts.weighting_scheme = scheme;
    knncolle::VptreeBuilder<> vpbuilder;

    // Checking for symmetry by flipping the matrix. The idea is that the
    // identities of the first and second nodes are flipped, but if symmetry of
    // the SNN calculations hold, then the original results should be recoverable
    // by just flipping the indices of the edge.
    std::vector<double> revdata(data.rbegin(), data.rend());
    auto revres = scran::build_snn_graph::compute(ndim, nobs, revdata.data(), vpbuilder, opts);
    auto revd = convert_to_vector(revres);

    for (auto& e : revd) {
        std::get<0>(e) = nobs - std::get<0>(e) - 1;
        std::get<1>(e) = nobs - std::get<1>(e) - 1;
        std::swap(std::get<0>(e), std::get<1>(e));
    }
    std::sort(revd.begin(), revd.end());

    auto refres = scran::build_snn_graph::compute(ndim, nobs, data.data(), vpbuilder, opts);
    auto refd = convert_to_vector(refres);
    std::sort(refd.begin(), refd.end());
    EXPECT_EQ(refd, revd);
}

INSTANTIATE_TEST_SUITE_P(
    BuildSnnGraph,
    BuildSnnGraphSymmetryTest,
    ::testing::Combine(
        ::testing::Values(4, 9), // number of neighbors
        ::testing::Values(scran::build_snn_graph::Scheme::RANKED, scran::build_snn_graph::Scheme::NUMBER, scran::build_snn_graph::Scheme::JACCARD) // weighting scheme
    )
);

/*****************************************
 *****************************************/

class BuildSnnGraphCustomNeighborsTest : public ::testing::TestWithParam<int>, public BuildSnnGraphTestCore {
protected:
    static void SetUpTestSuite() {
        assemble(20, 500); // dimensions, observations.
    }
};

TEST_P(BuildSnnGraphCustomNeighborsTest, Custom) {
    auto k = GetParam();

    scran::build_snn_graph::Options opts;
    opts.num_neighbors = k;
    knncolle::KmknnBuilder<> builder;
    auto output = scran::build_snn_graph::compute(ndim, nobs, data.data(), builder, opts);

    // Same results as if we had run it manually.
    auto prebuilt = builder.build_unique(knncolle::SimpleMatrix(ndim, nobs, data.data()));
    auto nn3 = knncolle::find_nearest_neighbors(*prebuilt, k);
    auto output3 = scran::build_snn_graph::compute(nn3, opts);
    EXPECT_EQ(output.edges, output3.edges);
    EXPECT_EQ(output.weights, output3.weights);
}

INSTANTIATE_TEST_SUITE_P(
    BuildSnnGraph,
    BuildSnnGraphCustomNeighborsTest,
    ::testing::Values(17, 32) // number of neighbors
);
