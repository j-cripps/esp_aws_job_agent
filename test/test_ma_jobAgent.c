/* test_mean.c: Implementation of a testable component.
   This example code is in the Public Domain (or CC0 licensed, at your option.)
   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "unity.h"
#include "ma_jobEngine.h"


TEST_CASE("Mean of an empty array is zero", "[ma_jobEngine]")
{
    const int values[] = { 0 };
    TEST_ASSERT_EQUAL(0, testable_mean(values, 0));
}
