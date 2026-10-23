from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.conditions import IfCondition
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node
from ament_index_python.packages import get_package_share_directory
import os

def generate_launch_description():
    share = get_package_share_directory('rslidar_sdk')
    return LaunchDescription([
        DeclareLaunchArgument('config_path', default_value=os.path.join(share, 'config', 'em4_online.yaml')),
        DeclareLaunchArgument('rviz', default_value='true'),
        Node(package='rslidar_sdk', executable='rslidar_sdk_node', output='screen',
             parameters=[{'config_path': LaunchConfiguration('config_path')}]),
        Node(package='rviz2', executable='rviz2', condition=IfCondition(LaunchConfiguration('rviz')),
             arguments=['-d', os.path.join(share, 'rviz', 'rviz2.rviz')]),
    ])
