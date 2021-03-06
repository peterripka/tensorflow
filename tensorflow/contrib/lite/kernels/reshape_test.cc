/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include <gtest/gtest.h>
#include "tensorflow/contrib/lite/interpreter.h"
#include "tensorflow/contrib/lite/kernels/register.h"
#include "tensorflow/contrib/lite/kernels/test_util.h"
#include "tensorflow/contrib/lite/model.h"

namespace tflite {
namespace {

using ::testing::ElementsAreArray;
using ::testing::IsEmpty;

// There are three ways to specify the output shape of a Reshape
// op.
enum ShapeSpecificationType {
  // The output shape is hardcoded in the ReshapeOptions object.
  kAsReshapeOption,
  // The output shape is specified as an input tensor, which is connected to a
  // Const node, which is guaranteed not to change once inference starts. The
  // shape is also hardcoded as in kAsReshapeOption.
  kAsConstantTensor,
  // The output shape is specifed as an input tensor that can change based on
  // external input. That is, the shape is not know before the inference
  // starts. The shape is also hardcoded as in kAsReshapeOption.
  kAsTensor,
};

class ReshapeOpTest
    : public ::testing::Test,
      public ::testing::WithParamInterface<ShapeSpecificationType> {};

class ReshapeOpModel : public SingleOpModel {
 public:
  ReshapeOpModel(std::initializer_list<int> input_shape,
                 std::initializer_list<int> shape_shape,
                 std::initializer_list<int> shape_data,
                 ShapeSpecificationType shape_type) {
    switch (shape_type) {
      case kAsTensor:
        BuildWithTensorShape(input_shape, shape_shape, shape_data);
        break;
      case kAsConstantTensor:
        BuildWithConstantTensorShape(input_shape, shape_shape, shape_data);
        break;
      case kAsReshapeOption:
        // In this case the shape of the new shape doesn't matter. It is
        // always hardcoded as a flat vector.
        BuildWithHardcodedShape(input_shape, shape_data);
        break;
    }
  }

  void SetInput(std::initializer_list<float> data) {
    PopulateTensor<float>(input_, data);
  }
  std::vector<float> GetOutput() { return ExtractVector<float>(output_); }
  std::vector<int> GetOutputShape() { return GetTensorShape(output_); }

 private:
  void BuildWithHardcodedShape(std::initializer_list<int> input_shape,
                               std::initializer_list<int> shape_data) {
    input_ = AddInput({TensorType_FLOAT32, input_shape});
    output_ = AddOutput(TensorType_FLOAT32);
    SetBuiltinOp(
        BuiltinOperator_RESHAPE, BuiltinOptions_ReshapeOptions,
        CreateReshapeOptions(builder_, builder_.CreateVector<int>(shape_data))
            .Union());
    BuildInterpreter({GetShape(input_)});
  }

  void BuildWithTensorShape(std::initializer_list<int> input_shape,
                            std::initializer_list<int> shape_shape,
                            std::initializer_list<int> shape_data) {
    input_ = AddInput({TensorType_FLOAT32, input_shape});
    output_ = AddOutput(TensorType_FLOAT32);
    int shape_input_tensor = AddInput({TensorType_INT32, shape_shape});
    // Note how shape also appears in ReshapeOptions
    SetBuiltinOp(
        BuiltinOperator_RESHAPE, BuiltinOptions_ReshapeOptions,
        CreateReshapeOptions(builder_, builder_.CreateVector<int>(shape_data))
            .Union());
    BuildInterpreter({GetShape(input_), GetShape(shape_input_tensor)});
    if (shape_data.size() != 0) {
      PopulateTensor<int32_t>(shape_input_tensor, shape_data);
    }
  }

  void BuildWithConstantTensorShape(std::initializer_list<int> input_shape,
                                    std::initializer_list<int> shape_shape,
                                    std::initializer_list<int> shape_data) {
    input_ = AddInput({TensorType_FLOAT32, input_shape});
    output_ = AddOutput(TensorType_FLOAT32);
    AddConstInput(TensorType_INT32, shape_data, shape_shape);
    // Note how the shape also appears in the ReshapeOptions.
    SetBuiltinOp(
        BuiltinOperator_RESHAPE, BuiltinOptions_ReshapeOptions,
        CreateReshapeOptions(builder_, builder_.CreateVector<int>(shape_data))
            .Union());
    BuildInterpreter({GetShape(input_)});
  }

  int input_;
  int output_;
};

TEST_P(ReshapeOpTest, MismatchedDimensions) {
  if (GetParam() == kAsTensor) {
    ReshapeOpModel m({1, 2, 4, 1}, {2}, {2, 1}, GetParam());
    m.SetInput({3});
    EXPECT_DEATH(m.Invoke(), "num_input_elements != num_output_elements");
  } else {
    EXPECT_DEATH(ReshapeOpModel({1, 2, 4, 1}, {2}, {2, 1}, GetParam()),
                 "num_input_elements != num_output_elements");
  }
}

TEST_P(ReshapeOpTest, TooManyDimensions) {
  if (GetParam() == kAsReshapeOption) {
    EXPECT_DEATH(ReshapeOpModel({1, 1, 2, 1, 1, 1, 1, 1, 1}, {9},
                                {1, 1, 1, 1, 1, 1, 1, 1, 2}, GetParam()),
                 "Found too many dimensions");
  } else {
    ReshapeOpModel m({1, 1, 2, 1, 1, 1, 1, 1, 1}, {9},
                     {1, 1, 1, 1, 1, 1, 1, 1, 2}, GetParam());
    m.SetInput({3, 4});
    m.Invoke();
    EXPECT_THAT(m.GetOutput(), ElementsAreArray({3, 4}));
    EXPECT_THAT(m.GetOutputShape(),
                ElementsAreArray({1, 1, 1, 1, 1, 1, 1, 1, 2}));
  }
}

TEST_P(ReshapeOpTest, TooManySpecialDimensions) {
  if (GetParam() != kAsTensor) {
    EXPECT_DEATH(ReshapeOpModel({1, 2, 4, 1}, {4}, {-1, -1, 2, 4}, GetParam()),
                 "stretch_dim != -1");
  } else {
    ReshapeOpModel m({1, 2, 4, 1}, {4}, {-1, -1, 2, 4}, GetParam());
    EXPECT_DEATH(m.Invoke(), "stretch_dim != -1");
  }
}

// Create the model with a 2x2 shape. Processing still works because the new
// shape ends up being hardcoded as a flat vector.
TEST_P(ReshapeOpTest, InvalidShape) {
  ReshapeOpModel m({1, 2, 2}, {2, 2}, {1, 2, 2, 1}, GetParam());
  m.SetInput({5, 6, 7, 8});
  m.Invoke();
  EXPECT_THAT(m.GetOutputShape(), ElementsAreArray({1, 2, 2, 1}));
  EXPECT_THAT(m.GetOutput(), ElementsAreArray({5, 6, 7, 8}));
}

// This is the normal scenario, where shape is a vector.
TEST_P(ReshapeOpTest, RegularShapes) {
  ReshapeOpModel m({1, 2, 4, 1}, {3}, {2, 2, 2}, GetParam());
  m.SetInput({1, 2, 3, 4, 5, 6, 7, 8});
  m.Invoke();
  EXPECT_THAT(m.GetOutput(), ElementsAreArray({1, 2, 3, 4, 5, 6, 7, 8}));
  EXPECT_THAT(m.GetOutputShape(), ElementsAreArray({2, 2, 2}));
}

TEST_P(ReshapeOpTest, WithStretchDimension) {
  ReshapeOpModel m({1, 2, 4, 1}, {3}, {2, 1, -1}, GetParam());
  m.SetInput({1, 2, 3, 4, 5, 6, 7, 8});
  m.Invoke();
  EXPECT_THAT(m.GetOutput(), ElementsAreArray({1, 2, 3, 4, 5, 6, 7, 8}));
  EXPECT_THAT(m.GetOutputShape(), ElementsAreArray({2, 1, 4}));
}

// Shape is specified as '[]', which is the modern way to represent scalar
// input and output.
TEST_P(ReshapeOpTest, ScalarOutput) {
  ReshapeOpModel m({1}, {0}, {}, GetParam());
  m.SetInput({3});
  m.Invoke();
  EXPECT_THAT(m.GetOutput(), ElementsAreArray({3}));
  EXPECT_THAT(m.GetOutputShape(), IsEmpty());
}

// Some old models specify '[0]' as the new shape, indicating that both input
// and output are scalars.
TEST_P(ReshapeOpTest, LegacyScalarOutput) {
  if (GetParam() == kAsConstantTensor) {
    EXPECT_DEATH(ReshapeOpModel({1}, {1}, {0}, GetParam()),
                 "num_input_elements != num_output_elements");
  } else if (GetParam() == kAsTensor) {
    ReshapeOpModel m({1}, {1}, {0}, GetParam());
    m.SetInput({3});
    EXPECT_DEATH(m.Invoke(), "num_input_elements != num_output_elements");
  } else {
    ReshapeOpModel m({1}, {1}, {0}, GetParam());
    m.SetInput({3});
    m.Invoke();
    EXPECT_THAT(m.GetOutput(), ElementsAreArray({3}));
    EXPECT_THAT(m.GetOutputShape(), IsEmpty());
  }
}

INSTANTIATE_TEST_CASE_P(VariedShapeSpec, ReshapeOpTest,
                        ::testing::Values(kAsReshapeOption, kAsConstantTensor,
                                          kAsTensor));
}  // namespace
}  // namespace tflite

int main(int argc, char** argv) {
  ::tflite::LogToStderr();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
