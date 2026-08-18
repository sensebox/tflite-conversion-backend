"""
Unit tests for the collect training images API endpoint.

Tests the /api/collect/images endpoint that compiles a sketch
for collecting training images with custom class names.
"""

import pytest
import json
import base64
import os
import sys
from unittest.mock import patch, MagicMock, Mock

# Add parent directory to path
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), '..')))

# Mock TensorFlow imports before importing app
sys.modules['tensorflow'] = Mock()
sys.modules['tensorflowjs'] = Mock()

from app import app
from services.model_injection import inject_class_names


@pytest.fixture
def client():
    """Create a test client for the Flask app."""
    app.config['TESTING'] = True
    with app.test_client() as client:
        yield client


class TestInjectClassNames:
    """Tests for inject_class_names function."""
    
    def test_inject_class_names_basic(self):
        """Test injecting basic class names."""
        class_names = ["cat", "dog", "bird"]
        sketch, error = inject_class_names(class_names)
        
        assert error is None
        assert sketch != ""
        
        # Check that all class names are in the sketch
        assert '"cat"' in sketch
        assert '"dog"' in sketch
        assert '"bird"' in sketch
        
        # Check that NUM_CLASSES is correct
        assert "const int NUM_CLASSES = 3;" in sketch
    
    def test_inject_class_names_single_class(self):
        """Test injecting a single class name."""
        class_names = ["object"]
        sketch, error = inject_class_names(class_names)
        
        assert error is None
        assert '"object"' in sketch
        assert "const int NUM_CLASSES = 1;" in sketch
    
    def test_inject_class_names_with_spaces(self):
        """Test injecting class names with spaces."""
        class_names = ["class 1", "class 2"]
        sketch, error = inject_class_names(class_names)
        
        assert error is None
        assert '"class 1"' in sketch
        assert '"class 2"' in sketch
    
    def test_inject_class_names_with_underscores(self):
        """Test injecting class names with underscores."""
        class_names = ["my_class", "another_class"]
        sketch, error = inject_class_names(class_names)
        
        assert error is None
        assert '"my_class"' in sketch
        assert '"another_class"' in sketch
    
    def test_inject_class_names_empty_list_fails(self):
        """Test that empty class names list fails."""
        class_names = []
        sketch, error = inject_class_names(class_names)
        
        assert error is not None
        assert "empty" in error.lower()
    
    def test_inject_class_names_not_list_fails(self):
        """Test that non-list input fails."""
        class_names = "cat"  # String instead of list
        sketch, error = inject_class_names(class_names)
        
        assert error is not None
    
    def test_inject_class_names_with_numbers(self):
        """Test injecting class names with numbers."""
        class_names = ["class1", "class2", "class3"]
        sketch, error = inject_class_names(class_names)
        
        assert error is None
        assert '"class1"' in sketch
        assert '"class2"' in sketch
        assert '"class3"' in sketch
    
    def test_inject_class_names_many_classes(self):
        """Test injecting many class names."""
        class_names = [f"class_{i}" for i in range(20)]
        sketch, error = inject_class_names(class_names)
        
        assert error is None
        assert "const int NUM_CLASSES = 20;" in sketch
        
        # Check a few class names
        assert '"class_0"' in sketch
        assert '"class_10"' in sketch
        assert '"class_19"' in sketch


class TestCollectImagesEndpoint:
    """Tests for the /api/collect/images endpoint."""
    
    def test_collect_images_missing_class_names(self, client):
        """Test that endpoint fails when classNames is missing."""
        response = client.post(
            '/api/collect/images',
            data=json.dumps({}),
            content_type='application/json'
        )
        
        assert response.status_code == 400
        data = json.loads(response.data)
        assert data['success'] is False
        # Either "empty request body" or "missing class names" is acceptable
        error_msg = data['error']['message'].lower()
        assert 'empty' in error_msg or 'missing' in error_msg or 'classnames' in error_msg
    
    def test_collect_images_empty_class_names(self, client):
        """Test that endpoint fails with empty classNames."""
        response = client.post(
            '/api/collect/images',
            data=json.dumps({'classNames': []}),
            content_type='application/json'
        )
        
        assert response.status_code == 400
        data = json.loads(response.data)
        assert data['success'] is False
        assert 'empty' in data['error']['message'].lower()
    
    def test_collect_images_invalid_class_names_type(self, client):
        """Test that endpoint fails when classNames is not a list."""
        response = client.post(
            '/api/collect/images',
            data=json.dumps({'classNames': 'not a list'}),
            content_type='application/json'
        )
        
        assert response.status_code == 400
        data = json.loads(response.data)
        assert data['success'] is False
    
    def test_collect_images_invalid_content_type(self, client):
        """Test that endpoint fails with invalid content type."""
        response = client.post(
            '/api/collect/images',
            data='not json',
            content_type='text/plain'
        )
        
        assert response.status_code == 400
        data = json.loads(response.data)
        assert data['success'] is False
        assert 'content type' in data['error']['message'].lower()
    
    def test_collect_images_empty_body(self, client):
        """Test that endpoint fails with empty body."""
        response = client.post(
            '/api/collect/images',
            data='',
            content_type='application/json'
        )
        
        # Could be 400 or 500 depending on JSON parsing
        assert response.status_code in [400, 500]
        data = json.loads(response.data)
        assert data['success'] is False
    
    @patch('services.compiler_service.compile_sketch')
    def test_collect_images_success(self, mock_compile, client):
        """Test successful compilation with class names."""
        # Mock the compiler to return a binary
        mock_binary = b'mock_binary_data' * 100
        mock_compile.return_value = mock_binary
        
        response = client.post(
            '/api/collect/images',
            data=json.dumps({
                'classNames': ['cat', 'dog', 'bird'],
                'boardType': 'sensebox_mcu_eye'  # Use valid board type
            }),
            content_type='application/json'
        )
        
        assert response.status_code == 200
        data = json.loads(response.data)
        
        assert data['success'] is True
        assert 'binaryData' in data['data']
        assert data['data']['numClasses'] == 3
        assert data['data']['classNames'] == ['cat', 'dog', 'bird']
        assert 'binarySize' in data['data']
        assert 'timestamp' in data['data']
    
    @patch('services.compiler_service.compile_sketch')
    def test_collect_images_with_board_option(self, mock_compile, client):
        """Test that board option is passed through."""
        mock_binary = b'mock_binary_data' * 100
        mock_compile.return_value = mock_binary
        
        response = client.post(
            '/api/collect/images',
            data=json.dumps({
                'classNames': ['a', 'b'],
                'boardType': 'sensebox'  # Use valid board type
            }),
            content_type='application/json'
        )
        
        assert response.status_code == 200
        data = json.loads(response.data)
        assert data['data']['board'] == 'sensebox'
    
    @patch('services.compiler_service.compile_sketch')
    def test_collect_images_with_optimization(self, mock_compile, client):
        """Test that optimization option is passed through."""
        mock_binary = b'mock_binary_data' * 100
        mock_compile.return_value = mock_binary
        
        response = client.post(
            '/api/collect/images',
            data=json.dumps({
                'classNames': ['class1', 'class2'],
                'optimization': 'size',
                'boardType': 'sensebox_mcu_eye'  # Use valid board type
            }),
            content_type='application/json'
        )
        
        assert response.status_code == 200
        data = json.loads(response.data)
        assert data['data']['optimization'] == 'size'
    
    def test_collect_images_invalid_optimization(self, client):
        """Test that invalid optimization level is rejected."""
        response = client.post(
            '/api/collect/images',
            data=json.dumps({
                'classNames': ['a', 'b'],
                'optimization': 'invalid'
            }),
            content_type='application/json'
        )
        
        assert response.status_code == 400
        data = json.loads(response.data)
        assert data['success'] is False
        assert 'optimization' in data['error']['message'].lower()
    
    @patch('services.compiler_service.compile_sketch')
    def test_collect_images_many_classes(self, mock_compile, client):
        """Test with many class names."""
        mock_binary = b'mock_binary_data' * 100
        mock_compile.return_value = mock_binary
        
        class_names = [f"class_{i}" for i in range(10)]
        response = client.post(
            '/api/collect/images',
            data=json.dumps({
                'classNames': class_names,
                'boardType': 'sensebox_mcu_eye'  # Use valid board type
            }),
            content_type='application/json'
        )
        
        assert response.status_code == 200
        data = json.loads(response.data)
        assert data['data']['numClasses'] == 10
        assert len(data['data']['classNames']) == 10
